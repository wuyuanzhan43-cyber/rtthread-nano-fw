/*
 * board_pins.c - STM32F407VE 板级接线描述与校验。
 *
 * 层级：src/platform/board/stm32f407ve
 * 职责：
 *   1. 从 board_config.h 构造 serial/SPI 描述符；
 *   2. 校验端口 0..4、SWD PA13/PA14、AF 组合和重复引脚。
 *
 * 上下文：调度器启动前。
 * 依赖：board_pins.h、board_config.h、fw_config.h。
 *
 * 注意：
 *   - 端口 0=A..4=E，对应 F407VE 100 脚封装；
 *   - AF 表只覆盖当前板级使用的 USART1..3、SPI1..3 常用引脚；
 *   - 校验失败由 board_devices_init 向上返回，最终启动 panic。
 */
#include "board_pins.h"
#include "board_config.h"
#include "fw_config.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t port;
    uint8_t pin;
    uint8_t af;
} board_af_t;

typedef struct {
    uint8_t port;
    uint8_t pin;
} board_pin_use_t;

/* USART 复用表（AF7）。 */
static const board_af_t s_uart1_tx[] = {{0U, 9U, 7U}, {1U, 6U, 7U}};
static const board_af_t s_uart1_rx[] = {{0U, 10U, 7U}, {1U, 7U, 7U}};
static const board_af_t s_uart2_tx[] = {{0U, 2U, 7U}, {3U, 5U, 7U}};
static const board_af_t s_uart2_rx[] = {{0U, 3U, 7U}, {3U, 6U, 7U}};
static const board_af_t s_uart3_tx[] = {{1U, 10U, 7U}, {2U, 10U, 7U},
                                        {3U, 8U, 7U}};
static const board_af_t s_uart3_rx[] = {{1U, 11U, 7U}, {2U, 11U, 7U},
                                        {3U, 9U, 7U}};

/* SPI 复用表（SPI1/2 AF5，SPI3 AF6）。 */
static const board_af_t s_spi1_sck[] = {{0U, 5U, 5U}, {1U, 3U, 5U}};
static const board_af_t s_spi1_mosi[] = {{0U, 7U, 5U}, {1U, 5U, 5U}};
static const board_af_t s_spi1_miso[] = {{0U, 6U, 5U}, {1U, 4U, 5U}};
static const board_af_t s_spi2_sck[] = {{1U, 13U, 5U}};
static const board_af_t s_spi2_mosi[] = {{1U, 15U, 5U}, {2U, 3U, 5U}};
static const board_af_t s_spi2_miso[] = {{1U, 14U, 5U}, {2U, 2U, 5U}};
static const board_af_t s_spi3_sck[] = {{1U, 3U, 6U}, {2U, 10U, 6U}};
static const board_af_t s_spi3_mosi[] = {{1U, 5U, 6U}, {2U, 12U, 6U}};
static const board_af_t s_spi3_miso[] = {{1U, 4U, 6U}, {2U, 11U, 6U}};

const drv_serial_hw_t *board_serial_hw(size_t *count)
{
#if (defined(FW_USING_UART1) && FW_USING_UART1) || \
    (defined(FW_USING_UART2) && FW_USING_UART2) || \
    (defined(FW_USING_UART3) && FW_USING_UART3)
    static const drv_serial_hw_t serial_hw[] = {
#if defined(FW_USING_UART1) && FW_USING_UART1
        {1U, BOARD_UART1_TX_PORT, BOARD_UART1_TX_PIN, BOARD_UART1_AF,
         BOARD_UART1_RX_PORT, BOARD_UART1_RX_PIN, BOARD_UART1_AF,
         BOARD_UART_BAUD, BOARD_UART_IRQ_PRIORITY},
#endif
#if defined(FW_USING_UART2) && FW_USING_UART2
        {2U, BOARD_UART2_TX_PORT, BOARD_UART2_TX_PIN, BOARD_UART2_AF,
         BOARD_UART2_RX_PORT, BOARD_UART2_RX_PIN, BOARD_UART2_AF,
         BOARD_UART_BAUD, BOARD_UART_IRQ_PRIORITY},
#endif
#if defined(FW_USING_UART3) && FW_USING_UART3
        {3U, BOARD_UART3_TX_PORT, BOARD_UART3_TX_PIN, BOARD_UART3_AF,
         BOARD_UART3_RX_PORT, BOARD_UART3_RX_PIN, BOARD_UART3_AF,
         BOARD_UART_BAUD, BOARD_UART_IRQ_PRIORITY},
#endif
    };
    if (count != NULL) *count = sizeof(serial_hw) / sizeof(serial_hw[0]);
    return serial_hw;
#else
    if (count != NULL) *count = 0U;
    return NULL;
#endif
}

const drv_spi_hw_t *board_spi_hw(size_t *count)
{
#if (defined(FW_USING_SPI1) && FW_USING_SPI1) || \
    (defined(FW_USING_SPI2) && FW_USING_SPI2) || \
    (defined(FW_USING_SPI3) && FW_USING_SPI3)
    static const drv_spi_hw_t spi_hw[] = {
#if defined(FW_USING_SPI1) && FW_USING_SPI1
        {1U,
         BOARD_SPI1_SCK_PORT, BOARD_SPI1_SCK_PIN, BOARD_SPI1_AF,
         BOARD_SPI1_MOSI_PORT, BOARD_SPI1_MOSI_PIN, BOARD_SPI1_AF,
         BOARD_SPI1_MISO_PORT, BOARD_SPI1_MISO_PIN, BOARD_SPI1_AF,
         BOARD_SPI1_USE_MISO != 0, BOARD_SPI1_MAX_HZ},
#endif
#if defined(FW_USING_SPI2) && FW_USING_SPI2
        {2U,
         BOARD_SPI2_SCK_PORT, BOARD_SPI2_SCK_PIN, BOARD_SPI2_AF,
         BOARD_SPI2_MOSI_PORT, BOARD_SPI2_MOSI_PIN, BOARD_SPI2_AF,
         BOARD_SPI2_MISO_PORT, BOARD_SPI2_MISO_PIN, BOARD_SPI2_AF,
         BOARD_SPI2_USE_MISO != 0, BOARD_SPI2_MAX_HZ},
#endif
#if defined(FW_USING_SPI3) && FW_USING_SPI3
        {3U,
         BOARD_SPI3_SCK_PORT, BOARD_SPI3_SCK_PIN, BOARD_SPI3_AF,
         BOARD_SPI3_MOSI_PORT, BOARD_SPI3_MOSI_PIN, BOARD_SPI3_AF,
         BOARD_SPI3_MISO_PORT, BOARD_SPI3_MISO_PIN, BOARD_SPI3_AF,
         BOARD_SPI3_USE_MISO != 0, BOARD_SPI3_MAX_HZ},
#endif
    };
    if (count != NULL) *count = sizeof(spi_hw) / sizeof(spi_hw[0]);
    return spi_hw;
#else
    if (count != NULL) *count = 0U;
    return NULL;
#endif
}

static bool pin_range_ok(uint8_t port, uint8_t pin)
{
    return port <= 4U && pin <= 15U;
}

static bool pin_reserved(uint8_t port, uint8_t pin)
{
    /* PA13/PA14 是 SWDIO/SWCLK，板级接线不能占用。 */
    return port == 0U && (pin == 13U || pin == 14U);
}

static bool af_in(const board_af_t *table, size_t count, uint8_t port,
                  uint8_t pin, uint8_t af)
{
    size_t i;
    for (i = 0U; i < count; ++i) {
        if (table[i].port == port && table[i].pin == pin &&
            table[i].af == af) return true;
    }
    return false;
}

static bool af_allowed(uint8_t instance, bool is_tx, uint8_t port,
                       uint8_t pin, uint8_t af)
{
    switch (instance) {
    case 1:
        if (is_tx) return af_in(s_uart1_tx, sizeof(s_uart1_tx) / sizeof(s_uart1_tx[0]), port, pin, af);
        return af_in(s_uart1_rx, sizeof(s_uart1_rx) / sizeof(s_uart1_rx[0]), port, pin, af);
    case 2:
        if (is_tx) return af_in(s_uart2_tx, sizeof(s_uart2_tx) / sizeof(s_uart2_tx[0]), port, pin, af);
        return af_in(s_uart2_rx, sizeof(s_uart2_rx) / sizeof(s_uart2_rx[0]), port, pin, af);
    case 3:
        if (is_tx) return af_in(s_uart3_tx, sizeof(s_uart3_tx) / sizeof(s_uart3_tx[0]), port, pin, af);
        return af_in(s_uart3_rx, sizeof(s_uart3_rx) / sizeof(s_uart3_rx[0]), port, pin, af);
    default:
        return false;
    }
}

static bool spi_af_allowed(uint8_t instance, uint8_t signal, uint8_t port,
                           uint8_t pin, uint8_t af)
{
    const board_af_t *table = NULL;
    size_t count = 0U;

    switch (instance) {
    case 1:
        if (signal == 0U) { table = s_spi1_sck; count = sizeof(s_spi1_sck) / sizeof(s_spi1_sck[0]); }
        else if (signal == 1U) { table = s_spi1_mosi; count = sizeof(s_spi1_mosi) / sizeof(s_spi1_mosi[0]); }
        else { table = s_spi1_miso; count = sizeof(s_spi1_miso) / sizeof(s_spi1_miso[0]); }
        break;
    case 2:
        if (signal == 0U) { table = s_spi2_sck; count = sizeof(s_spi2_sck) / sizeof(s_spi2_sck[0]); }
        else if (signal == 1U) { table = s_spi2_mosi; count = sizeof(s_spi2_mosi) / sizeof(s_spi2_mosi[0]); }
        else { table = s_spi2_miso; count = sizeof(s_spi2_miso) / sizeof(s_spi2_miso[0]); }
        break;
    case 3:
        if (signal == 0U) { table = s_spi3_sck; count = sizeof(s_spi3_sck) / sizeof(s_spi3_sck[0]); }
        else if (signal == 1U) { table = s_spi3_mosi; count = sizeof(s_spi3_mosi) / sizeof(s_spi3_mosi[0]); }
        else { table = s_spi3_miso; count = sizeof(s_spi3_miso) / sizeof(s_spi3_miso[0]); }
        break;
    default:
        return false;
    }
    return af_in(table, count, port, pin, af);
}

static bool add_pin(board_pin_use_t *used, size_t *count, size_t capacity,
                    uint8_t port, uint8_t pin)
{
    size_t i;

    if (!pin_range_ok(port, pin) || pin_reserved(port, pin)) return false;
    for (i = 0U; i < *count; ++i) {
        if (used[i].port == port && used[i].pin == pin) return false;
    }
    if (*count >= capacity) return false;
    used[*count].port = port;
    used[*count].pin = pin;
    ++(*count);
    return true;
}

fw_status_t board_pins_validate(void)
{
    board_pin_use_t used[24];
    size_t used_count = 0U;
    size_t count;
    const drv_serial_hw_t *serial = board_serial_hw(&count);
    const drv_spi_hw_t *spi = board_spi_hw(&count);
    size_t i;

    for (i = 0U; i < count; ++i) {
        const drv_spi_hw_t *hw = &spi[i];

        if (!add_pin(used, &used_count, 24U, hw->sck_port, hw->sck_pin) ||
            !add_pin(used, &used_count, 24U, hw->mosi_port, hw->mosi_pin))
            return FW_EINVAL;
        if (hw->use_miso &&
            !add_pin(used, &used_count, 24U, hw->miso_port, hw->miso_pin))
            return FW_EINVAL;
        if (!spi_af_allowed(hw->instance, 0U, hw->sck_port, hw->sck_pin,
                            hw->sck_af) ||
            !spi_af_allowed(hw->instance, 1U, hw->mosi_port, hw->mosi_pin,
                            hw->mosi_af) ||
            (hw->use_miso &&
             !spi_af_allowed(hw->instance, 2U, hw->miso_port, hw->miso_pin,
                             hw->miso_af)))
            return FW_EINVAL;
    }

    serial = board_serial_hw(&count);
    for (i = 0U; i < count; ++i) {
        const drv_serial_hw_t *hw = &serial[i];

        if (!add_pin(used, &used_count, 24U, hw->tx_port, hw->tx_pin) ||
            !add_pin(used, &used_count, 24U, hw->rx_port, hw->rx_pin))
            return FW_EINVAL;
        if (hw->tx_port == hw->rx_port && hw->tx_pin == hw->rx_pin)
            return FW_EINVAL;
        if (!af_allowed(hw->instance, true, hw->tx_port, hw->tx_pin,
                        hw->tx_af) ||
            !af_allowed(hw->instance, false, hw->rx_port, hw->rx_pin,
                        hw->rx_af))
            return FW_EINVAL;
    }

#if defined(BOARD_LED_ENABLE) && BOARD_LED_ENABLE
    if (!add_pin(used, &used_count, 24U, BOARD_LED_PORT, BOARD_LED_PIN))
        return FW_EINVAL;
#endif

#if defined(BOARD_LCD_ENABLE) && BOARD_LCD_ENABLE
    if (!add_pin(used, &used_count, 24U, BOARD_LCD_CS_PORT, BOARD_LCD_CS_PIN) ||
        !add_pin(used, &used_count, 24U, BOARD_LCD_DC_PORT, BOARD_LCD_DC_PIN) ||
        !add_pin(used, &used_count, 24U, BOARD_LCD_RES_PORT, BOARD_LCD_RES_PIN) ||
        !add_pin(used, &used_count, 24U, BOARD_LCD_BL_PORT, BOARD_LCD_BL_PIN))
        return FW_EINVAL;
#endif

    return FW_OK;
}
