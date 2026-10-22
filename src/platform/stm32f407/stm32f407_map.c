/*
 * stm32f407_map.c - STM32F407 实例映射实现。
 *
 * 层级：src/platform/stm32f407
 * 职责：
 *   1. 把数字端口/实例编号转换成 HAL 符号；
 *   2. 集中处理不同封装可能缺少某些 GPIO 端口的情况；
 *   3. 计算 SPI 分频。
 *
 * 上下文：初始化阶段；纯查表，无共享状态。
 * 依赖：stm32f407_map.h。
 *
 * 条件编译说明：
 *   GPIOF..GPIOI 用 #ifdef 保护，因为 100 脚 F407VE 只有 A..E，
 *   完整的 stm32f407xx.h 仍然定义了这些符号，但保留保护可以让
 *   同一份映射代码复用到更小封装而不报错。
 */
#include "stm32f407_map.h"

/*
 * 端口编号 -> 寄存器基址。
 * 0=A, 1=B, ..., 8=I；越界返回 NULL。
 */
GPIO_TypeDef *stm32f407_gpio(uint8_t port_index)
{
    switch (port_index) {
    case 0U: return GPIOA;
#ifdef GPIOB
    case 1U: return GPIOB;
#endif
#ifdef GPIOC
    case 2U: return GPIOC;
#endif
#ifdef GPIOD
    case 3U: return GPIOD;
#endif
#ifdef GPIOE
    case 4U: return GPIOE;
#endif
#ifdef GPIOF
    case 5U: return GPIOF;
#endif
#ifdef GPIOG
    case 6U: return GPIOG;
#endif
#ifdef GPIOH
    case 7U: return GPIOH;
#endif
#ifdef GPIOI
    case 8U: return GPIOI;
#endif
    default: return NULL;
    }
}

/*
 * 端口编号 -> 使能 RCC AHB1 时钟。
 *
 * 每个 GPIO 端口的时钟必须在使用前打开，否则寄存器写入无效。
 * 函数返回 FW_EINVAL 让调用者能够报告“端口不存在”，
 * 而不是静默地配置失败。
 */
fw_status_t stm32f407_gpio_clock_enable(uint8_t port_index)
{
    switch (port_index) {
    case 0U: __HAL_RCC_GPIOA_CLK_ENABLE(); return FW_OK;
#ifdef GPIOB
    case 1U: __HAL_RCC_GPIOB_CLK_ENABLE(); return FW_OK;
#endif
#ifdef GPIOC
    case 2U: __HAL_RCC_GPIOC_CLK_ENABLE(); return FW_OK;
#endif
#ifdef GPIOD
    case 3U: __HAL_RCC_GPIOD_CLK_ENABLE(); return FW_OK;
#endif
#ifdef GPIOE
    case 4U: __HAL_RCC_GPIOE_CLK_ENABLE(); return FW_OK;
#endif
#ifdef GPIOF
    case 5U: __HAL_RCC_GPIOF_CLK_ENABLE(); return FW_OK;
#endif
#ifdef GPIOG
    case 6U: __HAL_RCC_GPIOG_CLK_ENABLE(); return FW_OK;
#endif
#ifdef GPIOH
    case 7U: __HAL_RCC_GPIOH_CLK_ENABLE(); return FW_OK;
#endif
#ifdef GPIOI
    case 8U: __HAL_RCC_GPIOI_CLK_ENABLE(); return FW_OK;
#endif
    default: return FW_EINVAL;
    }
}

/* USART 编号 -> 寄存器基址；当前板级只支持 USART1..3。 */
USART_TypeDef *stm32f407_usart(uint8_t instance)
{
    switch (instance) {
    case 1U: return USART1;
    case 2U: return USART2;
    case 3U: return USART3;
    default: return NULL;
    }
}

/* USART 编号 -> IRQ 号；非法编号返回 -1，调用者用 (int)<0 判断。 */
IRQn_Type stm32f407_usart_irq(uint8_t instance)
{
    switch (instance) {
    case 1U: return USART1_IRQn;
    case 2U: return USART2_IRQn;
    case 3U: return USART3_IRQn;
    default: return (IRQn_Type)(-1);
    }
}

/* 使能 USART 时钟；不同实例分别位于 APB2/APB1，宏内部已处理。 */
void stm32f407_usart_clock_enable(uint8_t instance)
{
    switch (instance) {
    case 1U: __HAL_RCC_USART1_CLK_ENABLE(); break;
    case 2U: __HAL_RCC_USART2_CLK_ENABLE(); break;
    case 3U: __HAL_RCC_USART3_CLK_ENABLE(); break;
    default: break;
    }
}

/* SPI 编号 -> 寄存器基址。 */
SPI_TypeDef *stm32f407_spi(uint8_t instance)
{
    switch (instance) {
    case 1U: return SPI1;
    case 2U: return SPI2;
    case 3U: return SPI3;
    default: return NULL;
    }
}

/* SPI 编号 -> IRQ 号；当前轮询模式不使用，保留给 DMA/中断扩展。 */
IRQn_Type stm32f407_spi_irq(uint8_t instance)
{
    switch (instance) {
    case 1U: return SPI1_IRQn;
    case 2U: return SPI2_IRQn;
    case 3U: return SPI3_IRQn;
    default: return (IRQn_Type)(-1);
    }
}

/* 使能 SPI 时钟。 */
void stm32f407_spi_clock_enable(uint8_t instance)
{
    switch (instance) {
    case 1U: __HAL_RCC_SPI1_CLK_ENABLE(); break;
    case 2U: __HAL_RCC_SPI2_CLK_ENABLE(); break;
    case 3U: __HAL_RCC_SPI3_CLK_ENABLE(); break;
    default: break;
    }
}

/*
 * DMA stream 编号 -> CMSIS 寄存器块。
 *
 * 之所以用 switch 而不是基址+偏移，是因为 DMA1/DMA2 的 stream 寄存器
 * 是 0x18 字节步进的独立块，直接使用 CMSIS 符号更清晰，也避免把
 * 寄存器布局假设写死。
 */
DMA_Stream_TypeDef *stm32f407_dma_stream(uint8_t controller, uint8_t stream)
{
    if (controller == 1U) {
        switch (stream) {
        case 0U: return DMA1_Stream0;
        case 1U: return DMA1_Stream1;
        case 2U: return DMA1_Stream2;
        case 3U: return DMA1_Stream3;
        case 4U: return DMA1_Stream4;
        case 5U: return DMA1_Stream5;
        case 6U: return DMA1_Stream6;
        case 7U: return DMA1_Stream7;
        default: return NULL;
        }
    }
    if (controller == 2U) {
        switch (stream) {
        case 0U: return DMA2_Stream0;
        case 1U: return DMA2_Stream1;
        case 2U: return DMA2_Stream2;
        case 3U: return DMA2_Stream3;
        case 4U: return DMA2_Stream4;
        case 5U: return DMA2_Stream5;
        case 6U: return DMA2_Stream6;
        case 7U: return DMA2_Stream7;
        default: return NULL;
        }
    }
    return NULL;
}

/* DMA stream 编号 -> NVIC 中断号；非法组合返回 (IRQn_Type)-1。 */
IRQn_Type stm32f407_dma_irq(uint8_t controller, uint8_t stream)
{
    if (controller == 1U) {
        switch (stream) {
        case 0U: return DMA1_Stream0_IRQn;
        case 1U: return DMA1_Stream1_IRQn;
        case 2U: return DMA1_Stream2_IRQn;
        case 3U: return DMA1_Stream3_IRQn;
        case 4U: return DMA1_Stream4_IRQn;
        case 5U: return DMA1_Stream5_IRQn;
        case 6U: return DMA1_Stream6_IRQn;
        case 7U: return DMA1_Stream7_IRQn;
        default: return (IRQn_Type)(-1);
        }
    }
    if (controller == 2U) {
        switch (stream) {
        case 0U: return DMA2_Stream0_IRQn;
        case 1U: return DMA2_Stream1_IRQn;
        case 2U: return DMA2_Stream2_IRQn;
        case 3U: return DMA2_Stream3_IRQn;
        case 4U: return DMA2_Stream4_IRQn;
        case 5U: return DMA2_Stream5_IRQn;
        case 6U: return DMA2_Stream6_IRQn;
        case 7U: return DMA2_Stream7_IRQn;
        default: return (IRQn_Type)(-1);
        }
    }
    return (IRQn_Type)(-1);
}

/*
 * SPI 总线时钟。
 *
 * 在 STM32F407 的时钟树中：
 *   SPI1 位于 APB2，使用 PCLK2；
 *   SPI2/SPI3 位于 APB1，使用 PCLK1。
 * HAL_RCC_GetPCLKx 返回的是 APB 分频后的实际频率，
 * 已经考虑了 APB 预分频，因此可以直接用于分频计算。
 */
uint32_t stm32f407_spi_pclk(uint8_t instance)
{
    if (instance == 1U) return HAL_RCC_GetPCLK2Freq();
    return HAL_RCC_GetPCLK1Freq();
}

/*
 * 计算 SPI 分频。
 *
 * 先做向上取整除法：
 *   divider = (pclk + max_hz - 1) / max_hz
 * 得到“至少需要多少分频才能不超过 max_hz”；
 * 然后映射到 SPI 支持的 2 的幂分频。
 *
 * 例如 pclk=42MHz, max_hz=12MHz：
 *   divider = (42+11)/12 = 4 -> 4 分频 -> 10.5MHz。
 *
 * 如果 divider 超过 256，返回 256（最低速），保证不会出现
 * 未定义的分频值；调用者可以通过实际频率判断是否满足需求。
 */
uint32_t stm32f407_spi_prescaler(uint32_t pclk, uint32_t max_hz)
{
    uint32_t divider;
    if (max_hz == 0U) return SPI_BAUDRATEPRESCALER_2;
    divider = (pclk + max_hz - 1U) / max_hz;
    if (divider <= 2U) return SPI_BAUDRATEPRESCALER_2;
    if (divider <= 4U) return SPI_BAUDRATEPRESCALER_4;
    if (divider <= 8U) return SPI_BAUDRATEPRESCALER_8;
    if (divider <= 16U) return SPI_BAUDRATEPRESCALER_16;
    if (divider <= 32U) return SPI_BAUDRATEPRESCALER_32;
    if (divider <= 64U) return SPI_BAUDRATEPRESCALER_64;
    if (divider <= 128U) return SPI_BAUDRATEPRESCALER_128;
    return SPI_BAUDRATEPRESCALER_256;
}
