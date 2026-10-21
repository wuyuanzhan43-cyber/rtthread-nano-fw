/*
 * drv_common.h - STM32 驱动公共时间与状态辅助。
 *
 * 层级：src/platform/stm32f407
 * 职责：
 *   1. 让一个 SysTick 同时驱动 RT-Thread tick 和 HAL 的 uwTick；
 *   2. 提供可在启动早期/中断中使用的微秒忙等；
 *   3. 把 HAL 状态码映射成框架状态码。
 *
 * 上下文：
 *   - rt_hw_systick_init 在调度器启动前调用；
 *   - rt_hw_us_delay 可在启动前或 ISR 中使用（纯忙等，不切换线程）；
 *   - drv_status_from_hal 是纯函数。
 *
 * 依赖：stm32f4xx_hal.h、fw/status.h。
 *
 * 为什么需要这些适配：
 *   STM32 HAL 默认把 SysTick 当作自己的 1ms 时基，而 RT-Thread 也要求
 *   SysTick 驱动调度 tick。如果各自初始化，会出现 HAL_Delay 与 rt_tick
 *   不一致、SysTick 被重复配置的问题。这里统一由 RT-Thread 的 tick
 *   频率决定 SysTick 重装载值，HAL 只借用同一个中断累加 uwTick。
 */
#ifndef DRV_COMMON_H
#define DRV_COMMON_H

#include <stdint.h>
#include "stm32f4xx_hal.h"
#include "fw/status.h"

/*
 * 初始化 SysTick。
 *
 * 用 SystemCoreClock / RT_TICK_PER_SECOND 作为重装载值；
 * RT_TICK_PER_SECOND 当前为 1000，因此 1 tick = 1ms。
 * 同时把 SysTick 中断优先级设为最低（0xFF），
 * 保证串口等实时中断可以抢占 tick。
 *
 * 注意：HAL_InitTick 会调用本函数，board 初始化时也会显式调用一次；
 * 函数本身是幂等的，重复配置不会产生副作用。
 */
void rt_hw_systick_init(void);

/* 把 HAL_StatusTypeDef（HAL_OK/HAL_BUSY/HAL_TIMEOUT/HAL_ERROR）转成 fw_status_t。 */
fw_status_t drv_status_from_hal(HAL_StatusTypeDef status);

#endif
