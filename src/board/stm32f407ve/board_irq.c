/*
 * board_irq.c - 板级中断入口。
 *
 * 层级：src/board/stm32f407ve
 * 职责：提供 SysTick_Handler，让同一个 1ms 中断同时驱动
 *       HAL 的 uwTick 和 RT-Thread 的调度 tick。
 * 上下文：中断上下文。
 * 依赖：board.h、drv_common.h、rthw.h、rtthread.h。
 *
 * 其他中断的位置：
 *   - USARTx_IRQHandler 在 src/bsp/st/driver/drv_serial.c 中定义，
 *     只有对应 UART 启用时才生成；
 *   - HardFault/PendSV/SVC 由 RT-Thread libcpu 提供；
 *   - 其他外设中断若以后新增，应放在各自驱动中，而不是这个文件。
 *
 * 为什么 SysTick 放在 board：
 *   SysTick 是内核级时基，不属于某个具体外设；
 *   它需要同时调用 HAL 和 RT-Thread 两套接口，放在板级最合适。
 */
#include "board.h"
#include "drv_common.h"
#include <rthw.h>
#include <rtthread.h>

/*
 * SysTick 中断。
 *
 * 顺序：
 *   1. rt_interrupt_enter：通知 RT-Thread 进入中断，维护嵌套计数；
 *   2. HAL_IncTick：累加 HAL 毫秒计数（步长由 drv_common.c 决定）；
 *   3. rt_tick_increase：推进调度 tick，可能触发时间片/延时唤醒；
 *   4. rt_interrupt_leave：退出中断，必要时在中断返回时切换线程。
 *
 * 这里的顺序不影响功能，但保持“HAL 先、RT-Thread 后”可以让
 * 同一 tick 内 HAL 时间先更新，便于 HAL 超时判断。
 */
void SysTick_Handler(void)
{
    rt_interrupt_enter();
    HAL_IncTick();
    rt_tick_increase();
    rt_interrupt_leave();
}
