/*
 * stm32f407_boot.c - STM32F407 平台启动实现。
 *
 * 层级：src/platform/stm32f407
 * 职责：
 *   1. 在 HAL_Init 前捕获并清除 RCC 复位标志；
 *   2. HAL_Init、NVIC 分组、时钟、SysTick、延时钩子；
 *   3. 可选 RT-Thread 堆初始化；
 *   4. 暴露复位原因。
 *
 * 上下文：调度器启动前。
 * 依赖：stm32f407_boot.h、stm32f407_clock.h、drv_common.h、
 *       fw/clock.h、rthw.h、rtthread.h、stm32f4xx_hal.h。
 */
#include "stm32f407_boot.h"
#include "stm32f407_clock.h"
#include "drv_common.h"
#include "fw/clock.h"
#include <stdint.h>
#include <stm32f4xx_hal.h>
#include <rthw.h>
#include <rtthread.h>

#define RESET_CAUSE_MASK (RCC_CSR_BORRSTF | RCC_CSR_PINRSTF | \
                          RCC_CSR_PORRSTF | RCC_CSR_SFTRSTF | \
                          RCC_CSR_IWDGRSTF | RCC_CSR_WWDGRSTF | \
                          RCC_CSR_LPWRRSTF)

#ifdef RT_USING_HEAP
extern char _ebss;
extern char _estack;
#define STM32F407_MSP_RESERVE 4096U
#endif

static uint32_t s_reset_cause;

uint32_t stm32f407_reset_cause(void)
{
    return s_reset_cause;
}

const char *stm32f407_reset_cause_name(uint32_t cause)
{
    if ((cause & RCC_CSR_PORRSTF) != 0U) return "POR";
    if ((cause & RCC_CSR_BORRSTF) != 0U) return "BOR";
    if ((cause & RCC_CSR_PINRSTF) != 0U) return "PIN";
    if ((cause & RCC_CSR_SFTRSTF) != 0U) return "SFT";
    if ((cause & RCC_CSR_IWDGRSTF) != 0U) return "IWDG";
    if ((cause & RCC_CSR_WWDGRSTF) != 0U) return "WWDG";
    if ((cause & RCC_CSR_LPWRRSTF) != 0U) return "LPWR";
    return "UNKNOWN";
}

/* 调度器已启动且不在 ISR 时让出 CPU，否则忙等。 */
static void stm32f407_delay_ms(uint32_t ms)
{
    if (rt_thread_self() != RT_NULL && rt_interrupt_get_nest() == 0U) {
        rt_thread_mdelay(ms);
    } else {
        rt_hw_us_delay(ms * 1000U);
    }
}

fw_status_t stm32f407_boot_init(const stm32f407_clock_cfg_t *clock)
{
    if (clock == NULL) return FW_EINVAL;

    /* 必须在 HAL_Init 之前读取；读完后清标志，避免下次误判。 */
    s_reset_cause = RCC->CSR & RESET_CAUSE_MASK;
    RCC->CSR |= RCC_CSR_RMVF;

    if (HAL_Init() != HAL_OK) return FW_EIO;
    HAL_NVIC_SetPriorityGrouping(NVIC_PRIORITYGROUP_4);
    if (stm32f407_clock_init(clock) != FW_OK) return FW_EIO;
    rt_hw_systick_init();
    fw_clock_set_delay_fn(stm32f407_delay_ms);

#ifdef RT_USING_HEAP
    rt_system_heap_init(&_ebss,
                        (void *)((uintptr_t)&_estack - STM32F407_MSP_RESERVE));
#endif
    return FW_OK;
}
