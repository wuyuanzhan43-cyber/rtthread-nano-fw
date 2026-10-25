/*
 * stm32f407_map.h - STM32F407 外设实例映射接口。
 *
 * 层级：src/platform/stm32f407
 * 职责：
 *   1. 把 Kconfig 中的数字编号（端口 0=A、USART 1..3、SPI 1..3）
 *      解析成 HAL 的外设指针、IRQ 号和时钟使能动作；
 *   2. 提供 SPI 分频计算。
 *
 * 上下文：初始化阶段调用；纯查表/纯计算，无共享状态。
 * 依赖：stm32f4xx_hal.h、fw/status.h。
 *
 * 为什么集中在这里：
 *   如果每个驱动各自写 switch，新增实例或换芯片时要改很多文件；
 *   集中映射后，drv_serial/drv_spi 只使用统一的函数名，
 *   芯片差异被限制在 stm32f407_map.c 中。
 */
#ifndef STM32F407_MAP_H
#define STM32F407_MAP_H

#include <stdint.h>
#include "stm32f4xx_hal.h"
#include "fw/status.h"

/* 端口编号：0=A, 1=B, ..., 8=I；不存在的端口返回 NULL。 */
GPIO_TypeDef *stm32f407_gpio(uint8_t port_index);

/* 使能端口时钟；端口不存在时返回 FW_EINVAL。 */
fw_status_t stm32f407_gpio_clock_enable(uint8_t port_index);

/* USART 实例编号 1..3 -> 寄存器基址；非法编号返回 NULL。 */
USART_TypeDef *stm32f407_usart(uint8_t instance);

/* USART 实例编号 1..3 -> NVIC 中断号；非法编号返回 (IRQn_Type)-1。 */
IRQn_Type stm32f407_usart_irq(uint8_t instance);

/* 使能 USART 时钟；非法编号静默忽略（调用者应先校验实例）。 */
void stm32f407_usart_clock_enable(uint8_t instance);

/* SPI 实例编号 1..3 -> 寄存器基址；非法编号返回 NULL。 */
SPI_TypeDef *stm32f407_spi(uint8_t instance);

/* SPI 实例编号 1..3 -> NVIC 中断号（DMA 模式下用于错误中断）。 */
IRQn_Type stm32f407_spi_irq(uint8_t instance);

/* 使能 SPI 时钟；非法编号静默忽略。 */
void stm32f407_spi_clock_enable(uint8_t instance);

/* I2C 实例编号 1..3 -> 寄存器基址；非法编号返回 NULL。 */
I2C_TypeDef *stm32f407_i2c(uint8_t instance);

/* 使能 I2C 时钟；非法编号静默忽略。 */
void stm32f407_i2c_clock_enable(uint8_t instance);

/* ADC1 寄存器基址；当前芯片只映射 ADC1。 */
ADC_TypeDef *stm32f407_adc1(void);

/* 使能 ADC1 时钟（包含 ADC 公共时钟）。 */
void stm32f407_adc_clock_enable(void);

/* TIM4 寄存器基址；PWM 设备 pwm4 使用。 */
TIM_TypeDef *stm32f407_tim4(void);

/* 使能 TIM4 时钟。 */
void stm32f407_tim4_clock_enable(void);

/*
 * TIM4 的计数器时钟。
 * APB1 分频为 1 时等于 PCLK1，否则等于 2*PCLK1。
 * PWM 的 PSC/ARR 计算必须使用这个频率。
 */
uint32_t stm32f407_tim4_clk(void);

/*
 * DMA 映射：
 *   controller 1=DMA1、2=DMA2；stream 0..7。
 * dma_stream 返回 HAL 需要写入 DMA_HandleTypeDef.Instance 的寄存器块；
 * dma_irq 返回该 stream 的 NVIC 中断号。
 * 非法组合分别返回 NULL / (IRQn_Type)-1。
 *
 * 板级 DMA 路由（哪个外设用哪个 controller/stream/channel）在
 * board_config.h 的 C 表中手写；这里只做“编号 -> CMSIS 符号”的纯映射，
 * 不包含板级策略。
 */
DMA_Stream_TypeDef *stm32f407_dma_stream(uint8_t controller, uint8_t stream);
IRQn_Type stm32f407_dma_irq(uint8_t controller, uint8_t stream);

/*
 * 返回 SPI 所在总线的时钟频率：
 *   SPI1 挂在 APB2（PCLK2）；
 *   SPI2/SPI3 挂在 APB1（PCLK1）。
 * 分频计算必须用对应总线的频率，不能用 SYSCLK。
 */
uint32_t stm32f407_spi_pclk(uint8_t instance);

/*
 * 选择不超过 max_hz 的最小分频，返回 SPI_BAUDRATEPRESCALER_x。
 *
 * 计算：
 *   divider = ceil(pclk / max_hz)
 *   然后向上取到 2/4/8/.../256 中最小的合法值。
 * 例如 pclk=84MHz、max_hz=12MHz -> divider=7 -> 选 8 分频，
 * 实际 10.5MHz，不超过 12MHz。
 * max_hz=0 时返回最小分频 2，避免除零。
 */
uint32_t stm32f407_spi_prescaler(uint32_t pclk, uint32_t max_hz);

#endif
