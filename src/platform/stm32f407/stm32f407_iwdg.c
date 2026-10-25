/*
 * stm32f407_iwdg.c - STM32F407 独立看门狗。
 *
 * 层级：src/platform/stm32f407
 * 职责：
 *   1. 用 LSI 32kHz 估算预分频与重装值；
 *   2. 调用 HAL_IWDG_Init 启动；
 *   3. 提供无副作用的喂狗接口。
 *
 * 上下文：任务/启动上下文；HAL_IWDG_Refresh 可在任务中调用。
 * 依赖：stm32f407_iwdg.h、stm32f4xx_hal.h。
 *
 * 注意：IWDG 一旦启动不能停止；调试时若在断点停留超过超时时间，
 * 芯片会复位。需要长时间调试时关闭 BOARD_IWDG_ENABLE。
 */
#include "stm32f407_iwdg.h"
#include "stm32f4xx_hal.h"
#include <stdbool.h>

#define IWDG_LSI_HZ 32000U
#define IWDG_RELOAD_MAX 0x0FFFU

static IWDG_HandleTypeDef s_iwdg;
static bool s_started;

/* 预分频档位与 LSI 分频比。 */
static const uint32_t s_prescalers[] = {
    IWDG_PRESCALER_4,
    IWDG_PRESCALER_8,
    IWDG_PRESCALER_16,
    IWDG_PRESCALER_32,
    IWDG_PRESCALER_64,
    IWDG_PRESCALER_128,
    IWDG_PRESCALER_256
};

static const uint32_t s_dividers[] = {4U, 8U, 16U, 32U, 64U, 128U, 256U};

fw_status_t stm32f407_iwdg_start(uint32_t timeout_ms)
{
    uint32_t index;
    uint32_t reload = 0U;
    uint32_t prescaler = 0U;
    bool found = false;

    if (timeout_ms == 0U) return FW_EINVAL;

    /*
     * timeout = reload * divider / LSI
     * => reload = timeout_ms * LSI / (1000 * divider)
     * 选第一个 reload 能放进 12 位的档位。
     */
    for (index = 0U; index < sizeof(s_dividers) / sizeof(s_dividers[0]);
         ++index) {
        uint64_t value = (uint64_t)timeout_ms * IWDG_LSI_HZ /
                         (1000ULL * s_dividers[index]);
        if (value <= IWDG_RELOAD_MAX) {
            reload = value == 0U ? 1U : (uint32_t)value;
            prescaler = s_prescalers[index];
            found = true;
            break;
        }
    }
    if (!found) return FW_EINVAL;

    s_iwdg.Instance = IWDG;
    s_iwdg.Init.Prescaler = prescaler;
    s_iwdg.Init.Reload = reload;
    if (HAL_IWDG_Init(&s_iwdg) != HAL_OK) return FW_EIO;

    s_started = true;
    return FW_OK;
}

void stm32f407_iwdg_feed(void)
{
    if (!s_started) return;
    (void)HAL_IWDG_Refresh(&s_iwdg);
}
