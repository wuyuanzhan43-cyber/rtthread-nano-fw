/*
 * board_clock.c - STM32F407VE 板级时钟描述实现。
 *
 * 层级：src/platform/board/stm32f407ve
 * 职责：
 *   - HSI 16MHz：SYSCLK=16MHz，APB1/APB2 不分频，Flash latency=0；
 *   - HSE：按 BOARD_HSE_VALUE 计算 PLLM，PLLN=336、PLLP=2、PLLQ=7，
 *     AHB=/1、APB1=/4、APB2=/2，目标 SYSCLK=168MHz、Flash latency=5。
 *
 * 上下文：启动阶段。
 * 依赖：board_clock.h、board_config.h。
 */
#include "board_clock.h"
#include "board_config.h"

#if BOARD_CLOCK_HSE_8_168
#if (BOARD_HSE_VALUE % 1000000) != 0
#error "BOARD_HSE_VALUE must be an integer multiple of 1MHz"
#endif
#if (BOARD_HSE_VALUE / 1000000) < 2 || (BOARD_HSE_VALUE / 1000000) > 63
#error "BOARD_HSE_VALUE / 1MHz must fit PLLM range 2..63"
#endif

static const stm32f407_clock_cfg_t s_clock = {
    true,
    BOARD_HSE_VALUE,
    (uint8_t)(BOARD_HSE_VALUE / 1000000U),
    336U,
    2U,
    7U,
    1U,
    4U,
    2U,
    5U
};
#else
static const stm32f407_clock_cfg_t s_clock = {
    false,
    0U,
    0U,
    0U,
    0U,
    0U,
    1U,
    1U,
    1U,
    0U
};
#endif

const stm32f407_clock_cfg_t *board_clock_config(void)
{
    return &s_clock;
}
