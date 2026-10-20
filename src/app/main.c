/*
 * main.c - v2 框架的组合根（composition root）。
 *
 * 层级：src/app
 * 职责：
 *   1. 从 board 层取得控制台串口、pin 设备和 LED 引脚号；
 *   2. 打开设备（触发类的 open 和驱动 configure）；
 *   3. 启动串口中断、绘制 LCD 启动画面；
 *   4. 初始化 demo 应用并进入“等待 RX + 轮询”的主循环。
 *
 * 上下文：RT-Thread main 线程。
 * 依赖：board.h、fw/demo_app.h、fw/rtthread_clock.h、fw/status.h。
 *
 * 设计说明：
 *   main.c 不直接访问 USART/GPIO 寄存器，也不包含 HAL 头文件；
 *   它只认识 board 提供的设备句柄和 fw_dev_* 接口。
 *   这样应用逻辑可以在 Host 上用假设备跑单元测试。
 *
 * 初始化顺序为什么重要：
 *   board_devices_init() 已经在 rt_hw_board_init() 中注册了 pin、
 *   serial1..N、spi1..N 和 lcd；此时硬件已经初始化，但 USART 中断
 *   尚未打开。main 先 open（配置串口/清 ring），再 board_console_start()
 *   打开中断，避免在 ring 尚未准备好时收到数据。
 */
#include <rtthread.h>
#include "board.h"
#include "fw/demo_app.h"
#include "fw/rtthread_clock.h"
#include "fw/status.h"

/*
 * 程序入口。
 *
 * 返回类型是 int，但正常路径永不返回：主循环一直运行。
 * 任何初始化失败都打印原因并进入 1 秒一次的“安全停靠”循环，
 * 方便通过串口日志定位问题，而不是继续使用未初始化的设备。
 */
int main(void)
{
    fw_demo_app_t application;
    fw_demo_config_t config;
    fw_status_t status;

    /* 启动横幅：此时控制台可能仍处于轮询输出模式，见 board_console.c。 */
    rt_kprintf("v2 framework: RT-Thread Nano + mr-style devices\n");

    /*
     * 组装应用配置。
     *
     * board_console_dev() 返回当前 BOARD_CONSOLE_UART_NUM 对应的 serialN；
     * board_pin_dev() 返回唯一的 "pin" 设备；
     * board_led_pin() 返回 Kconfig 配置的编码引脚号；
     * fw_rtthread_clock() 把 rt_tick_get 包装成与 RTOS 无关的毫秒时钟。
     */
    config.serial = board_console_dev();
    config.pin = board_pin_dev();
    config.led_pin = board_led_pin();
    config.led_active_high = board_led_active_high();
    config.clock = fw_rtthread_clock();

    /* 控制台串口是必需的：没有它就无法输出诊断信息。 */
    if (config.serial == NULL) {
        rt_kprintf("device lookup failed: console serial\n");
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
        rt_kprintf("console open failed: %s\n", fw_status_name(status));
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
            rt_kprintf("pin open failed: %s\n", fw_status_name(status));
            for (;;) rt_thread_mdelay(1000U);
        }
    }

    /*
     * 打开所有已注册串口的中断。
     *
     * 这一步之后 RXNE/TXE/ERR 中断才会生效；
     * 之前 fw_dev_write 只能把数据放进 ring，无法真正发送。
     */
    status = board_console_start();
    if (status != FW_OK) {
        rt_kprintf("console start failed: %s\n", fw_status_name(status));
        for (;;) rt_thread_mdelay(1000U);
    }

    /*
     * 绘制 LCD 启动画面。
     *
     * FW_ENOTSUP 表示编译时关闭了 LCD，属于正常情况，不打印错误；
     * 其他错误（SPI 超时、引脚配置失败）只记录，不阻止应用继续运行，
     * 因为显示不是核心功能。
     */
    status = board_boot_display();
    if (status != FW_OK && status != FW_ENOTSUP) {
        rt_kprintf("lcd init failed: %s\n", fw_status_name(status));
    }

    /*
     * 初始化 demo 应用：
     *   - 绑定 serial/pin/LED/clock；
     *   - 设置半帧超时、TX 超时和单轮 RX 预算；
     *   - 设置 1 秒一次的主动状态上报。
     */
    status = fw_demo_init(&application, &config);
    if (status == FW_OK) {
        status = fw_demo_set_limits(&application, 250U, 20U, 64U);
    }
    if (status == FW_OK) {
        status = fw_demo_set_report_period(&application, FW_DEMO_REPORT_PERIOD_MS);
    }
    if (status != FW_OK) {
        rt_kprintf("demo init failed: %s\n", fw_status_name(status));
        for (;;) rt_thread_mdelay(1000U);
    }

    /*
     * 主循环。
     *
     * 每一轮：
     *   1. fw_demo_poll 处理 RX、命令、TX 和周期上报；
     *   2. next_wait_ms 计算距离最近的定时任务还有多久；
     *   3. 用 fw_serial_wait_rx 睡眠等待：
     *        - 有串口数据 -> 事件唤醒，立即下一轮；
     *        - 有上报/半帧超时 -> 超时唤醒；
     *        - 都没有 -> 最多睡 1 秒，保持系统有周期性心跳。
     *
     * 注意：wait_rx 只是等待“有数据”，不会消费数据；
     * 真正的读取在下一轮 fw_demo_poll 中完成。
     */
    for (;;) {
        uint32_t wait;
        (void)fw_demo_poll(&application);
        wait = fw_demo_next_wait_ms(&application);
        if (wait == FW_DEMO_WAIT_FOREVER) wait = 1000U;
        (void)fw_serial_wait_rx(config.serial, wait);
    }
}
