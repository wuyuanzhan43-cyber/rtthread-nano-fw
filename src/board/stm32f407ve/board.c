/*
 * board.c - STM32F407VE 板级启动与 RT-Thread 移植入口。
 *
 * 层级：src/board/stm32f407ve
 * 职责：
 *   1. 初始化 HAL、时钟、SysTick；
 *   2. 安装框架延时钩子；
 *   3. 可选择地初始化 RT-Thread 堆；
 *   4. 注册板级设备；
 *   5. 执行 RT-Thread 组件初始化（如果启用）。
 *
 * 上下文：rt_hw_board_init 在调度器启动前、main 线程创建之前执行。
 * 依赖：board.h、board_config.h、stm32f407_clock.h、drv_common.h、
 *       fw/clock.h、rthw.h、rtthread.h。
 *
 * 启动顺序（从上电到 main）：
 *
 *   Reset_Handler
 *     -> SystemInit（启动文件，设置基础时钟）
 *     -> entry（RT-Thread libcpu）
 *        -> rtthread_startup
 *           -> rt_hw_board_init   <-- 本文件
 *           -> rt_system_scheduler_start
 *              -> main 线程开始运行
 *
 * 为什么设备注册放在调度器启动前：
 *   注册只涉及静态表和 HAL 初始化，不依赖线程/信号量；
 *   提前完成可以保证 main 第一次查找设备时一定成功。
 *   UART 中断在 board_console_start 中才打开，避免在设备未就绪时
 *   进入 ISR。
 */
#include "board.h"
#include "board_config.h"
#include "stm32f407_clock.h"
#include "drv_common.h"
#include "fw/clock.h"
#include <stdint.h>
#include <rthw.h>
#include <rtthread.h>

/*
 * 可选的 RT-Thread 堆初始化。
 *
 * 默认 RT_USING_HEAP=n，所有对象静态分配；如果将来打开堆，
 * 把堆放在 .bss 结束到栈顶之间，并保留 4KB 给主栈/中断栈。
 * 当前配置不会编译这段代码。
 */
#ifdef RT_USING_HEAP
extern char _ebss;
extern char _estack;
#define BOARD_MSP_RESERVE 4096U
#endif

/* panic 现场：调试器可以读取这两个变量定位失败位置。 */
volatile const char *g_board_panic_file;
volatile unsigned g_board_panic_line;

/*
 * 致命错误处理。
 *
 * 关闭所有可屏蔽中断，保存现场，然后原地空转。
 * 不复位是为了保留寄存器/内存状态，方便用调试器分析；
 * 产品固件若需要看门狗复位，应在这之前喂狗或等待看门狗溢出。
 */
void board_panic(const char *file, unsigned line)
{
    __disable_irq();
    g_board_panic_file = file;
    g_board_panic_line = line;
    for (;;) __NOP();
}

/*
 * 板级毫秒延时。
 *
 * 判断当前上下文：
 *   - 调度器已启动且不在中断中：rt_thread_mdelay，让出 CPU；
 *   - 启动早期或 ISR：不能用阻塞 API，改用 SysTick 忙等。
 *
 * 这个函数被安装为 fw_delay_ms 的钩子，ST7735S 初始化时使用。
 * 这样 framework/components 不需要直接包含 rtthread.h。
 */
static void board_delay_ms(uint32_t ms)
{
    if (rt_thread_self() != RT_NULL && rt_interrupt_get_nest() == 0U) {
        rt_thread_mdelay(ms);
    } else {
        rt_hw_us_delay(ms * 1000U);
    }
}

/*
 * RT-Thread 板级初始化入口。
 *
 * 步骤：
 *   1. HAL_Init：复位外设、初始化 Flash 接口和 SysTick 默认配置；
 *   2. 设置 NVIC 优先级分组为 4 位抢占、0 位子优先级；
 *   3. 配置系统时钟（HSI 或 HSE）；
 *   4. 用最终时钟重新配置 SysTick；
 *   5. 安装框架延时钩子；
 *   6. 可选地初始化堆；
 *   7. 注册板级设备；
 *   8. 执行组件初始化（如果 RT_USING_COMPONENTS_INIT）。
 *
 * 任何一步失败都会 panic；启动阶段没有控制台可用来打印错误，
 * 因此保存文件和行号是唯一的诊断手段。
 */
void rt_hw_board_init(void)
{
    fw_status_t result;

    HAL_Init();
    HAL_NVIC_SetPriorityGrouping(NVIC_PRIORITYGROUP_4);
    if (stm32f407_clock_init() != FW_OK) board_panic(__FILE__, __LINE__);
    rt_hw_systick_init();
    fw_clock_set_delay_fn(board_delay_ms);

#ifdef RT_USING_HEAP
    /*
     * 堆位于 .bss 末尾到栈顶之间；
     * 保留 BOARD_MSP_RESERVE 给主栈和中断栈，避免堆与栈相撞。
     */
    rt_system_heap_init(&_ebss,
                        (void *)((uintptr_t)&_estack - BOARD_MSP_RESERVE));
#endif

    /* 注册 pin/serial/spi/lcd，具体顺序见 board_devices.c。 */
    result = board_devices_init();
    if (result != FW_OK) board_panic(__FILE__, __LINE__);

#ifdef RT_USING_COMPONENTS_INIT
    /* 如果启用组件自动初始化，在这里执行 INIT_BOARD_EXPORT 等段。 */
    rt_components_board_init();
#endif
}
