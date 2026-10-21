/*
 * stm32f4xx_hal_conf.h - v2 的 HAL 模块裁剪与全局参数。
 *
 * 层级：src/platform/stm32f407
 * 职责：
 *   1. 只启用框架实际使用的 HAL 模块，减少 Flash 占用；
 *   2. 根据 Kconfig 的 BOARD_HSE_VALUE 配置外部晶振频率；
 *   3. 把 HAL 的 assert_param 钩子接到 stm32f407_panic；
 *   4. 关闭 HAL 内部的 RTOS 适配（USE_RTOS=0），由 bsp 自己集成 RT-Thread。
 *
 * 上下文：纯编译期配置。
 * 依赖：board_config.h（由 Kconfig 生成）。
 *
 * 说明：
 *   这个文件替换了 STM32Cube 默认的 stm32f4xx_hal_conf.h。
 *   未启用的模块（ADC、CAN、I2C、RTC 等）不会参与编译，
 *   对应源文件也没有加入 project/sources.json。
 */
#ifndef STM32F4XX_HAL_CONF_H
#define STM32F4XX_HAL_CONF_H

/* HSE_VALUE 是 HAL 的编译期输入，继续消费板级生成配置。 */
#include "board_config.h"
#include "stm32f407_panic.h"

/*
 * 启用的 HAL 模块。
 *
 * HAL_MODULE_ENABLED 是总开关；
 * GPIO/RCC/CORTEX 是基础设施；
 * UART/SPI 是框架设备；
 * DMA 虽然当前 UART 不用，但 HAL 的 SPI/UART 头文件可能引用其类型；
 * FLASH/PWR 用于时钟/电压配置。
 *
 * 新增外设类时，在这里打开对应模块，并把源文件加入 manifest。
 */
#define HAL_MODULE_ENABLED
#define HAL_GPIO_MODULE_ENABLED
#define HAL_RCC_MODULE_ENABLED
#define HAL_CORTEX_MODULE_ENABLED
#define HAL_UART_MODULE_ENABLED
#define HAL_SPI_MODULE_ENABLED
#define HAL_DMA_MODULE_ENABLED
#define HAL_FLASH_MODULE_ENABLED
#define HAL_PWR_MODULE_ENABLED

/*
 * 外部晶振频率。
 *
 * BOARD_HSE_VALUE 由 Kconfig 生成：
 *   - 选择 HSE 8MHz 时为 8000000；
 *   - 选择 HSI 时为 0。
 *
 * 即使使用 HSI，HAL 头文件仍要求定义 HSE_VALUE，
 * 这里给 8MHz 作为“板子可能焊了 8MHz 晶振”的默认值。
 */
#ifndef BOARD_HSE_VALUE
#define BOARD_HSE_VALUE 0
#endif

#if BOARD_HSE_VALUE > 0
#define HSE_VALUE ((uint32_t)BOARD_HSE_VALUE)
#else
#define HSE_VALUE 8000000U
#endif

/* HSE 启动超时（毫秒）；100ms 足够常见 8MHz 晶振起振。 */
#define HSE_STARTUP_TIMEOUT 100U

/* 内部 RC 频率；STM32F407 的 HSI 固定为 16MHz。 */
#define HSI_VALUE 16000000U

/* 内部低速 RC 与外部低速晶振频率，保留给 RTC/IWDG 未来使用。 */
#define LSI_VALUE 32000U
#define LSE_VALUE 32768U
#define LSE_STARTUP_TIMEOUT 5000U

/* 外部时钟（如以太网 PHY）参考值；当前未使用。 */
#define EXTERNAL_CLOCK_VALUE 12288000U

/* 供电电压（mV），HAL 用于内部参数计算。 */
#define VDD_VALUE 3300U

/* HAL 默认 tick 优先级；实际 SysTick 优先级由 rt_hw_systick_init 设置。 */
#define TICK_INT_PRIORITY 15U

/*
 * USE_RTOS=0 的原因：
 *   本项目不使用 HAL 自带的 RTOS 适配层（如 osMessageQueue 映射），
 *   串口/SPI 的并发由 BSP 和 RT-Thread 直接管理。
 *   如果设为 1，HAL 会尝试包含 cmsis_os2.h，增加不必要的依赖。
 */
#define USE_RTOS 0U

/* ART 加速器/缓存，F407 上默认启用，可提高 Flash 执行效率。 */
#define PREFETCH_ENABLE 1U
#define INSTRUCTION_CACHE_ENABLE 1U
#define DATA_CACHE_ENABLE 1U

/* 按启用模块包含对应 HAL 头文件；顺序与 CubeMX 生成结果一致。 */
#include "stm32f4xx_hal_rcc.h"
#include "stm32f4xx_hal_gpio.h"
#include "stm32f4xx_hal_cortex.h"
#include "stm32f4xx_hal_dma.h"
#include "stm32f4xx_hal_uart.h"
#include "stm32f4xx_hal_spi.h"
#include "stm32f4xx_hal_flash.h"
#include "stm32f4xx_hal_pwr.h"

/*
 * assert_param 重定向。
 *
 * HAL 在参数检查失败时调用 assert_param；默认在非 DEBUG 下是空宏。
 * 这里把它接到 stm32f407_panic：记录文件名/行号并停机，
 * 让“SPI 初始化参数错误”这类问题在开发阶段立即暴露，
 * 而不是悄悄继续运行。
 */
#define assert_param(expr) \
    ((expr) ? (void)0 : stm32f407_panic(__FILE__, __LINE__))

#endif
