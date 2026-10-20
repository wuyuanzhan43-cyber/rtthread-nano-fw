/*
 * drv_common.c - RT-Thread 与 STM32 HAL 的时间基共存。
 *
 * 层级：src/bsp/st/driver
 * 职责：
 *   1. 一个 SysTick 同时驱动 rt_tick 和 HAL uwTick；
 *   2. 重写 HAL_InitTick/HAL_Delay 等时间函数，避免两套时基冲突；
 *   3. 提供基于 SysTick 计数器的微秒忙等。
 *
 * 上下文：
 *   - rt_hw_systick_init 在调度器启动前；
 *   - SysTick_Handler 在中断中同时调用 HAL_IncTick 和 rt_tick_increase；
 *   - rt_hw_us_delay 可在启动前或 ISR 中使用，不允许阻塞。
 *
 * 依赖：drv_common.h、rtthread.h、rthw.h、stm32f4xx_hal.h。
 *
 * 时基约定：
 *   RT_TICK_PER_SECOND = 1000（Kconfig 可改），因此 SysTick 每 1ms
 *   中断一次；HAL 的 uwTick 也按同样步长累加。
 *   如果以后把 tick 改成 100Hz，systick_ms 会自动变成 10，
 *   HAL_GetTick 仍然返回毫秒。
 */
#include "drv_common.h"
#include <rthw.h>
#include <rtthread.h>

/* HAL 的毫秒计数，定义在 stm32f4xx_hal.c 中。 */
extern __IO uint32_t uwTick;

/* 每次 SysTick 中断对应的毫秒数；1000Hz 时为 1。 */
static uint32_t systick_ms = 1U;

/*
 * 配置 SysTick 为 RT-Thread 的 tick 源。
 *
 * SystemCoreClockUpdate() 根据当前 RCC 配置重新计算 SystemCoreClock，
 * 必须在时钟树初始化之后调用。
 *
 * HAL_SYSTICK_Config 设定重装载值和中断使能；
 * NVIC 优先级设为 0xFF（最低），让 USART 等中断可以抢占。
 * systick_ms 根据 tick 频率计算，供 HAL_IncTick 使用。
 */
void rt_hw_systick_init(void)
{
    SystemCoreClockUpdate();
    HAL_SYSTICK_Config(SystemCoreClock / RT_TICK_PER_SECOND);
    NVIC_SetPriority(SysTick_IRQn, 0xFFU);
    systick_ms = 1000U / RT_TICK_PER_SECOND;
    if (systick_ms == 0U) systick_ms = 1U;
}

/*
 * HAL tick 递增。
 *
 * HAL 默认每次 SysTick 加 1，这里改成加 systick_ms，
 * 保证在非 1000Hz 的 tick 配置下 uwTick 仍然以毫秒为单位。
 * 由 board_irq.c 的 SysTick_Handler 调用。
 */
void HAL_IncTick(void)
{
    uwTick += systick_ms;
}

/*
 * 读取 HAL 毫秒计数。
 *
 * 如果 SysTick 的 COUNTFLAG 置位（表示自上次读取以来至少溢出一次），
 * 主动补一次递增，避免在中断被长时间关闭时 uwTick 落后。
 * 读取 COUNTFLAG 会清除该标志。
 */
uint32_t HAL_GetTick(void)
{
    if ((SysTick->CTRL & SysTick_CTRL_COUNTFLAG_Msk) != 0U) {
        HAL_IncTick();
    }
    return uwTick;
}

/*
 * HAL 的 tick 初始化钩子。
 *
 * HAL_Init() 内部会调用它；我们忽略 HAL 传入的优先级，
 * 统一走 rt_hw_systick_init，确保 HAL 和 RT-Thread 使用同一个
 * SysTick 配置，而不是各配一套。
 */
HAL_StatusTypeDef HAL_InitTick(uint32_t tick_priority)
{
    (void)tick_priority;
    rt_hw_systick_init();
    return HAL_OK;
}

/*
 * 挂起/恢复 tick。
 *
 * 当前项目不使用 HAL 的低功耗 tick 挂起功能，保留为空实现，
 * 避免 HAL 内部调用时链接失败或意外关闭 RT-Thread 的调度时基。
 */
void HAL_SuspendTick(void)
{
}

void HAL_ResumeTick(void)
{
}

/*
 * HAL 毫秒延时。
 *
 * 两种上下文：
 *   - 调度器已启动且不在中断中：使用 rt_thread_mdelay，让出 CPU；
 *   - 启动早期或 ISR：不能用阻塞 API，改用 SysTick 忙等。
 *
 * 这个判断也保护了 LCD 初始化早期（调度器尚未运行）的调用。
 */
void HAL_Delay(__IO uint32_t delay)
{
    if (rt_thread_self() != RT_NULL && rt_interrupt_get_nest() == 0U) {
        rt_thread_mdelay(delay);
    } else {
        uint32_t count;
        for (count = 0U; count < delay; ++count) rt_hw_us_delay(1000U);
    }
}

/*
 * 微秒级忙等。
 *
 * 原理：
 *   SysTick 是一个向下计数的 24 位计数器，LOAD 为重装载值；
 *   VAL 每次内核时钟减少 1。连续读取 VAL，累计“减少过的计数”，
 *   直到达到目标微秒数对应的 tick 数。
 *
 * ticks_per_us 的计算：
 *   RT_TICK_PER_SECOND 表示每秒多少个 SysTick 中断；
 *   reload/ticks_per_us 近似等于“每微秒 SysTick 计数多少次”。
 *   这里假设 reload 与 RT_TICK_PER_SECOND 匹配（由 rt_hw_systick_init 保证）。
 *
 * 回绕处理：
 *   如果 tnow < told，说明计数器回绕，累计 reload - tnow + told；
 *   否则累计 told - tnow。
 *
 * 注意：这是忙等，会占满 CPU 且不响应同优先级任务；
 * 只用于启动早期、ISR 或极短延时。
 */
void rt_hw_us_delay(rt_uint32_t us)
{
    uint64_t ticks;
    uint32_t told;
    uint32_t tnow;
    uint32_t tcnt = 0U;
    uint32_t reload = SysTick->LOAD;
    uint32_t ticks_per_us = 1000000U / RT_TICK_PER_SECOND;

    if (ticks_per_us == 0U) ticks_per_us = 1U;
    ticks = (uint64_t)us * (reload / ticks_per_us);
    told = SysTick->VAL;
    for (;;) {
        tnow = SysTick->VAL;
        if (tnow != told) {
            if (tnow < told) tcnt += told - tnow;
            else tcnt += reload - tnow + told;
            told = tnow;
            if (tcnt >= ticks) break;
        }
    }
}

/*
 * HAL 状态到框架状态的映射。
 *
 * HAL_BUSY 通常是外设正忙，对应可重试的 FW_EBUSY；
 * HAL_TIMEOUT 对应 FW_ETIMEOUT；
 * 其余错误统一视为 IO 错误。
 */
fw_status_t drv_status_from_hal(HAL_StatusTypeDef status)
{
    switch (status) {
    case HAL_OK: return FW_OK;
    case HAL_BUSY: return FW_EBUSY;
    case HAL_TIMEOUT: return FW_ETIMEOUT;
    default: return FW_EIO;
    }
}
