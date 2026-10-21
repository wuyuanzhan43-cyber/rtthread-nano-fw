/*
 * stm32f407_clock.c - STM32F407 数据驱动时钟树实现。
 *
 * 层级：src/platform/stm32f407
 * 职责：
 *   1. 把板级 clock_cfg 映射成 HAL RCC 常量；
 *   2. 配置 HSI 或 HSE+PLL；
 *   3. 设置 AHB/APB 分频与 Flash latency；
 *   4. 更新 SystemCoreClock。
 *
 * 上下文：启动阶段、调度器启动前。
 * 依赖：stm32f407_clock.h、stm32f4xx_hal.h。
 */
#include "stm32f407_clock.h"
#include <stm32f4xx_hal.h>

static uint32_t map_ahb_div(uint16_t divider)
{
    switch (divider) {
    case 1:   return RCC_SYSCLK_DIV1;
    case 2:   return RCC_SYSCLK_DIV2;
    case 4:   return RCC_SYSCLK_DIV4;
    case 8:   return RCC_SYSCLK_DIV8;
    case 16:  return RCC_SYSCLK_DIV16;
    case 64:  return RCC_SYSCLK_DIV64;
    case 128: return RCC_SYSCLK_DIV128;
    case 256: return RCC_SYSCLK_DIV256;
    case 512: return RCC_SYSCLK_DIV512;
    default:  return 0xFFFFFFFFU;
    }
}

static uint32_t map_apb_div(uint8_t divider)
{
    switch (divider) {
    case 1:  return RCC_HCLK_DIV1;
    case 2:  return RCC_HCLK_DIV2;
    case 4:  return RCC_HCLK_DIV4;
    case 8:  return RCC_HCLK_DIV8;
    case 16: return RCC_HCLK_DIV16;
    default: return 0xFFFFFFFFU;
    }
}

static uint32_t map_pll_p(uint8_t divider)
{
    switch (divider) {
    case 2: return RCC_PLLP_DIV2;
    case 4: return RCC_PLLP_DIV4;
    case 6: return RCC_PLLP_DIV6;
    case 8: return RCC_PLLP_DIV8;
    default: return 0xFFFFFFFFU;
    }
}

fw_status_t stm32f407_clock_init(const stm32f407_clock_cfg_t *cfg)
{
    RCC_OscInitTypeDef oscillator = {0};
    RCC_ClkInitTypeDef clock = {0};
    uint32_t ahb;
    uint32_t apb1;
    uint32_t apb2;

    if (cfg == NULL || cfg->flash_latency > 15U) return FW_EINVAL;

    ahb = map_ahb_div(cfg->ahb_div);
    apb1 = map_apb_div(cfg->apb1_div);
    apb2 = map_apb_div(cfg->apb2_div);
    if (ahb == 0xFFFFFFFFU || apb1 == 0xFFFFFFFFU ||
        apb2 == 0xFFFFFFFFU) return FW_EINVAL;

    if (cfg->use_hse) {
        uint32_t pllp = map_pll_p(cfg->pll_p);

        if (cfg->hse_hz < 1000000U || pllp == 0xFFFFFFFFU ||
            cfg->pll_m < 2U || cfg->pll_m > 63U ||
            cfg->pll_n < 50U || cfg->pll_n > 432U ||
            cfg->pll_q < 2U || cfg->pll_q > 15U) return FW_EINVAL;
        if (cfg->hse_hz != HSE_VALUE) return FW_EINVAL;

        __HAL_RCC_PWR_CLK_ENABLE();
        __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

        oscillator.OscillatorType = RCC_OSCILLATORTYPE_HSE;
        oscillator.HSEState = RCC_HSE_ON;
        oscillator.PLL.PLLState = RCC_PLL_ON;
        oscillator.PLL.PLLSource = RCC_PLLSOURCE_HSE;
        oscillator.PLL.PLLM = cfg->pll_m;
        oscillator.PLL.PLLN = cfg->pll_n;
        oscillator.PLL.PLLP = pllp;
        oscillator.PLL.PLLQ = cfg->pll_q;
        if (HAL_RCC_OscConfig(&oscillator) != HAL_OK) return FW_EIO;

        clock.ClockType = RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK |
                          RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
        clock.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
        clock.AHBCLKDivider = ahb;
        clock.APB1CLKDivider = apb1;
        clock.APB2CLKDivider = apb2;
        if (HAL_RCC_ClockConfig(&clock, cfg->flash_latency) != HAL_OK)
            return FW_EIO;
    } else {
        oscillator.OscillatorType = RCC_OSCILLATORTYPE_HSI;
        oscillator.HSIState = RCC_HSI_ON;
        oscillator.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
        oscillator.PLL.PLLState = RCC_PLL_NONE;
        if (HAL_RCC_OscConfig(&oscillator) != HAL_OK) return FW_EIO;

        clock.ClockType = RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK |
                          RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
        clock.SYSCLKSource = RCC_SYSCLKSOURCE_HSI;
        clock.AHBCLKDivider = ahb;
        clock.APB1CLKDivider = apb1;
        clock.APB2CLKDivider = apb2;
        if (HAL_RCC_ClockConfig(&clock, cfg->flash_latency) != HAL_OK)
            return FW_EIO;
    }

    SystemCoreClockUpdate();
    return FW_OK;
}
