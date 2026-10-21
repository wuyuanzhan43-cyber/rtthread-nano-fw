/*
 * stm32f407_irq.c - STM32F407 平台中断入口。
 *
 * 层级：src/platform/stm32f407
 * 职责：SysTick 同时驱动 HAL uwTick、RT-Thread tick 和任务 CPU 采样。
 * 上下文：中断上下文。
 * 依赖：drv_common.h、fw/task.h、rthw.h、rtthread.h。
 *
 * 其他中断：
 *   - USARTx_IRQHandler 在 drv_serial.c；
 *   - HardFault/PendSV/SVC 由 RT-Thread libcpu 提供；
 *   - 板级外设中断若以后新增，放在 board 目录。
 */
#include "drv_common.h"
#include "fw/task.h"
#include <rthw.h>
#include <rtthread.h>

void SysTick_Handler(void)
{
    rt_interrupt_enter();
    fw_task_cpu_tick();
    HAL_IncTick();
    rt_tick_increase();
    rt_interrupt_leave();
}
