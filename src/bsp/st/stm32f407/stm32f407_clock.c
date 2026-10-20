/*
 * stm32f407_clock.c - STM32F407 时钟树实现。
 *
 * 层级：src/bsp/st/stm32f407
 * 职责：根据 Kconfig 把系统时钟配置为 HSI 16MHz 或 HSE 168MHz。
 * 上下文：调度器启动前；失败由 board 停机并记录。
 * 依赖：stm32f407_clock.h、stm32f4xx_hal.h、board_config.h。
 *
 * HSE 168MHz 配置推导：
 *   HSE = 8MHz
 *   PLLM = 8   -> PLL 输入 1MHz（VCO 输入建议 1..2MHz）
 *   PLLN = 336 -> VCO = 336MHz
 *   PLLP = 2   -> SYSCLK = 168MHz
 *   PLLQ = 7   -> 48MHz（USB/SDIO 时钟，本项目暂未使用但保持标准值）
 *
 *   总线分频：
 *     AHB  = /1 -> HCLK  = 168MHz
 *     APB1 = /4 -> PCLK1 = 42MHz（USART2/3、SPI2/3）
 *     APB2 = /2 -> PCLK2 = 84MHz（USART1、SPI1）
 *     Flash 等待周期 = 5（168MHz 下必须）
 *
 * HSI 16MHz 配置：
 *   SYSCLK 直接使用 HSI，不经过 PLL；
 *   APB1/APB2 不分频，Flash 等待 0。
 *   优点是无需外部晶振；缺点是 RC 精度较低，高波特率可能有误差。
 */
#include "stm32f407_clock.h"
#include "stm32f4xx_hal.h"
#include "board_config.h"

fw_status_t stm32f407_clock_init(void)
{
    RCC_OscInitTypeDef oscillator = {0};
    RCC_ClkInitTypeDef clock = {0};

#if BOARD_CLOCK_HSE_8_168
    /*
     * HSE 路径。
     *
     * 先打开 PWR 时钟并设置电压等级 1（168MHz 必须），
     * 然后配置 HSE + PLL 振荡器。
     */
    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

    oscillator.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    oscillator.HSEState = RCC_HSE_ON;
    oscillator.PLL.PLLState = RCC_PLL_ON;
    oscillator.PLL.PLLSource = RCC_PLLSOURCE_HSE;
    oscillator.PLL.PLLM = 8U;
    oscillator.PLL.PLLN = 336U;
    oscillator.PLL.PLLP = RCC_PLLP_DIV2;
    oscillator.PLL.PLLQ = 7U;
    if (HAL_RCC_OscConfig(&oscillator) != HAL_OK) return FW_EIO;

    /*
     * 切换 SYSCLK 到 PLL，并设置 AHB/APB 分频。
     * FLASH_LATENCY_5 是 168MHz、3.3V 下的推荐等待周期。
     */
    clock.ClockType = RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK |
                      RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    clock.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    clock.AHBCLKDivider = RCC_SYSCLK_DIV1;
    clock.APB1CLKDivider = RCC_HCLK_DIV4;
    clock.APB2CLKDivider = RCC_HCLK_DIV2;
    if (HAL_RCC_ClockConfig(&clock, FLASH_LATENCY_5) != HAL_OK) return FW_EIO;
#else
    /*
     * HSI 路径（默认）。
     *
     * 不启用 PLL：PLLState=NONE，SYSCLK 直接来自 HSI。
     * 16MHz 下 Flash 不需要等待周期，APB1/APB2 也不分频。
     */
    oscillator.OscillatorType = RCC_OSCILLATORTYPE_HSI;
    oscillator.HSIState = RCC_HSI_ON;
    oscillator.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
    oscillator.PLL.PLLState = RCC_PLL_NONE;
    if (HAL_RCC_OscConfig(&oscillator) != HAL_OK) return FW_EIO;

    clock.ClockType = RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK |
                      RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    clock.SYSCLKSource = RCC_SYSCLKSOURCE_HSI;
    clock.AHBCLKDivider = RCC_SYSCLK_DIV1;
    clock.APB1CLKDivider = RCC_HCLK_DIV1;
    clock.APB2CLKDivider = RCC_HCLK_DIV1;
    if (HAL_RCC_ClockConfig(&clock, FLASH_LATENCY_0) != HAL_OK) return FW_EIO;
#endif

    /*
     * 让 HAL 和后续代码拿到新的 SystemCoreClock。
     * 所有基于 SystemCoreClock 的计算（SysTick、波特率）都依赖这一步。
     */
    SystemCoreClockUpdate();
    return FW_OK;
}
