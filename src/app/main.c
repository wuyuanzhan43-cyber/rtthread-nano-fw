/*
 * main.c - v2 框架的组合根（composition root）与任务监督者。
 *
 * 层级：src/app
 * 职责：
 *   1. 从 board 层取得控制台串口、pin 设备和 LED 引脚号；
 *   2. 打开设备（触发类的 open 和驱动 configure）；
 *   3. 启动串口中断、绘制 LCD 启动画面；
 *   4. 初始化任务管理器和 app_rpc 静态任务；
 *   5. 进入低频监督循环：刷新健康度并按最近截止时间睡眠。
 *
 * 上下文：RT-Thread main 线程（优先级 10，栈 4096）。
 * 依赖：board.h、app/boot.h、app/rpc.h、app/tasks.h、
 *       fw/rtthread_clock.h、fw/status.h。
 *
 * 线程模型：
 *   - main：初始化 + fw_task_refresh 监督；
 *   - rpc：app_rpc_poll + fw_serial_wait_rx；
 *   - idle：内核线程，只参与 CPU idle 统计。
 *
 * 为什么 RPC 不在 main 里：
 *   任务管理器需要至少一个真实托管任务；把 RPC 循环拆出去后，
 *   main 可以专职刷新健康度，RPC 任务也有独立栈、CPU 和心跳。
 */
#include <rtthread.h>
#include "board.h"
#include "app/boot.h"
#include "app/rpc.h"
#include "app/tasks.h"
#include "fw/log.h"
#include "fw/rtthread_clock.h"
#include "fw/rtthread_log.h"
#include "fw/status.h"
#include "fw/task.h"

/*
 * 程序入口。
 *
 * 返回类型是 int，但正常路径永不返回：监督循环一直运行。
 * 任何初始化失败都打印原因并进入 1 秒一次的“安全停靠”循环。
 */
int main(void)
{
    app_rpc_config_t config;
    fw_status_t status;

    /* 绑定 fw_log sink；之后统一走 [级别] 前缀格式。 */
    fw_rtthread_log_bind();

    /* 启动横幅：board 名称、版本和上次复位原因。 */
    FW_LOGI("main", "v2 framework: %s %s, reset=%s",
            board_name(), board_version(),
            board_reset_cause_name(board_reset_cause()));

    /*
     * 组装应用配置。
     *
     * board_console_dev() 返回 BOARD_CONSOLE_UART 对应的 serialN；
     * board_pin_dev() 返回唯一的 "pin" 设备；
     * board_led_enabled() 为 false 时应用不绑定 LED 输出；
     * fw_rtthread_clock() 把 rt_tick_get 包装成与 RTOS 无关的毫秒时钟。
     */
    config.serial = board_console_dev();
    config.pin = board_led_enabled() ? board_pin_dev() : NULL;
    config.led_pin = board_led_enabled() ? board_led_pin() : -1;
    config.led_active_high = board_led_enabled() ? board_led_active_high()
                                                 : false;
    config.clock = fw_rtthread_clock();
    config.tasks = NULL;

    /* 控制台串口是必需的：没有它就无法输出诊断信息。 */
    if (config.serial == NULL) {
        FW_LOGE("main", "device lookup failed: console serial\n");
        for (;;) rt_thread_mdelay(1000U);
    }

    /*
     * 打开控制台串口。
     *
     * 第一次 open 会调用 serial_open：
     *   - 清空 RX/TX ring；
     *   - 调用驱动的 configure，按当前配置初始化 USART。
     * 注意此时中断还没打开，数据不会丢失在 ring 之外。
     */
    status = fw_dev_open(config.serial, FW_OFLAG_RDWR);
    if (status != FW_OK) {
        FW_LOGE("main", "console open failed: %s\n", fw_status_name(status));
        for (;;) rt_thread_mdelay(1000U);
    }

    /*
     * 打开 pin 设备。
     *
     * pin 类没有 open 动作，这里主要增加引用计数并做接口一致性检查；
     * 引脚模式已经在 board_devices_init() 中配置过。
     */
    if (config.pin != NULL) {
        status = fw_dev_open(config.pin, FW_OFLAG_RDWR);
        if (status != FW_OK) {
            FW_LOGE("main", "pin open failed: %s\n", fw_status_name(status));
            for (;;) rt_thread_mdelay(1000U);
        }
    }

    /*
     * 打开所有已注册串口的中断。
     *
     * 这一步之后 RXNE/TXE/ERR 中断才会生效；
     * 之前 fw_dev_write 只能把数据放进 ring，无法真正发送。
     */
    status = board_serial_start_all();
    if (status != FW_OK) {
        FW_LOGE("main", "serial start failed: %s\n", fw_status_name(status));
        for (;;) rt_thread_mdelay(1000U);
    }

    /*
     * 绘制 LCD 启动画面。
     *
     * 启动画面由应用层负责；board 只提供 lcd 设备。
     * FW_ENOTSUP 表示编译时关闭了 LCD，属于正常情况，不打印错误；
     * 其他错误（SPI 超时、引脚配置失败）只记录，不阻止应用继续运行。
     */
    status = app_boot_display(board_lcd());
    if (status != FW_OK && status != FW_ENOTSUP) {
        FW_LOGE("main", "lcd init failed: %s\n", fw_status_name(status));
    }

    /*
     * 初始化任务管理器：
     *   - adopt main（不可控、无心跳）；
     *   - 创建 rpc 任务（优先级 9、栈 APP_RPC_TASK_STACK_SIZE）；
     *   - 此时 rpc 任务尚未启动。
     */
    status = app_tasks_init();
    if (status != FW_OK) {
        FW_LOGE("main", "task init failed: %s\n", fw_status_name(status));
        for (;;) rt_thread_mdelay(1000U);
    }

    /*
     * 初始化应用层 RPC 服务：
     *   - 绑定 serial/pin/LED/clock 和任务管理器；
     *   - 设置半帧超时、TX 超时和单轮 RX 预算；
     *   - 设置 1 秒一次的主动状态上报。
     */
    config.tasks = app_tasks_manager();
    status = app_rpc_init(app_tasks_rpc(), &config);
    if (status == FW_OK) {
        status = app_rpc_set_limits(app_tasks_rpc(), 250U, 20U, 64U);
    }
    if (status == FW_OK) {
        status = app_rpc_set_report_period(app_tasks_rpc(),
                                           APP_RPC_REPORT_PERIOD_MS);
    }
    if (status != FW_OK) {
        FW_LOGE("main", "rpc init failed: %s\n", fw_status_name(status));
        for (;;) rt_thread_mdelay(1000U);
    }

    /* 启动 rpc 任务；此后串口由它独占。 */
    status = app_tasks_start();
    if (status != FW_OK) {
        FW_LOGE("main", "rpc task start failed: %s\n", fw_status_name(status));
        for (;;) rt_thread_mdelay(1000U);
    }

    /*
     * 所有设备和任务就绪后才启动 IWDG。
     *
     * 未启用时 board_watchdog_start 返回 FW_OK；启用后不可停止，
     * 必须由下面的监督循环按任务健康度喂狗。
     */
    status = board_watchdog_start();
    if (status != FW_OK) {
        FW_LOGE("main", "watchdog start failed: %s\n", fw_status_name(status));
    } else if (board_watchdog_enabled()) {
        FW_LOGI("main", "watchdog: enabled\n");
    }

    /*
     * 监督循环。
     *
     * 每轮刷新任务健康度（栈/状态/心跳/CPU），只有没有任务 LOST 时才
     * 喂狗；某个任务失联或 main 自身卡死都会让 IWDG 复位系统。
     * main 心跳关闭，因此不会因为自身休眠而被判 lost。
     */
    for (;;) {
        fw_task_summary_t summary;
        uint32_t wait;

        fw_task_refresh(app_tasks_manager());
        if (fw_task_summary(app_tasks_manager(), &summary) == FW_OK &&
            summary.lost == 0U) {
            board_watchdog_feed();
        }

        wait = fw_task_next_wait_ms(app_tasks_manager());
        if (wait == 0U) wait = 1U;
        rt_thread_mdelay((rt_int32_t)wait);
    }
}
