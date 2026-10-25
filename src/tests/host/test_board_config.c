/*
 * test_board_config.c - 手写板级 C 表的 Host 结构测试。
 *
 * 层级：src/tests/host
 *
 * 背景：
 *   board_config.h 是整块板子的接线事实源。它故意只依赖
 *   <stdbool.h>/<stdint.h>/board_policy.h，因此可以在 PC 上直接
 *   包含，不需要 HAL、RT-Thread 或真实硬件。
 *
 * 职责：
 *   1. 验证表结构：端口/pin 范围、SWD 保留脚、重复引脚、通道唯一、
 *      DMA 路由范围、LCD 实例范围；
 *   2. 回归关键接线：控制台 UART、LED、LCD、ADC、PWM、NVM 的
 *      默认值一旦被误改，测试立即失败；
 *   3. 检查内存常量自洽：代码区 + NVM = 总 Flash，NVM 紧接代码区。
 *
 * 不覆盖：
 *   - AF 是否真的属于某个外设（需要数据手册/上板验证）；
 *   - 电气时序、DMA 请求映射是否正确；
 *   - 运行时 GPIO 冲突（由 board.c: board_pins_validate() 覆盖）。
 *
 * 上下文：单线程；由 test_main.c 调用。
 * 依赖：board_config.h、stdio.h。
 */
#include <stdio.h>
#include "board_config.h"

/* test_main.c 中的全局失败计数。 */
extern int test_failures;

/*
 * 与 test_common.h 的 CHECK 等价，但不引入 host_devices/protocol 头。
 *
 * 这样本文件可以保持“只包含 board_config.h”的纯度，验证板级头确实
 * 没有偷偷依赖框架/OS 类型。
 */
#define CHECK(condition)                                                      \
    do {                                                                      \
        if (!(condition)) {                                                   \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);       \
            ++test_failures;                                                  \
        }                                                                     \
    } while (0)

/*
 * 用与固件完全相同的 INIT 宏实例化表。
 *
 * 不能直接检查宏文本，必须展开成数组，才能验证指定初始化器、
 * 字段顺序和数组长度。
 */
static const board_pin_t s_pins[BOARD_PIN_COUNT] = BOARD_PIN_TABLE_INIT;
static const board_uart_t s_uarts[] = BOARD_UART_TABLE_INIT;
static const board_spi_t s_spis[] = BOARD_SPI_TABLE_INIT;
static const board_i2c_t s_i2cs[] = BOARD_I2C_TABLE_INIT;
static const board_adc_t s_adcs[] = BOARD_ADC_TABLE_INIT;
static const board_pwm_t s_pwms[] = BOARD_PWM_TABLE_INIT;
static const board_lcd_t s_lcd = BOARD_LCD_INIT;

/* 计算静态数组元素个数；表新增行时无需改测试的计数。 */
#define COUNT(array) (sizeof(array) / sizeof((array)[0]))

/*
 * GPIO 是否合法：
 *   - F407VE 100 脚封装只有 GPIOA..GPIOE，port 0..4；
 *   - 每个端口 16 脚，pin 0..15；
 *   - PA13/PA14 是 SWDIO/SWCLK，不能作为板级功能脚。
 *
 * 与 board.c: pin_valid() 保持同一规则；如果两处规则不一致，
 * Host 测试和启动校验会给出不同结论，属于需要立即修复的缺陷。
 */
static bool pin_ok(board_pin_t pin)
{
    if (pin.port > 4U || pin.pin > 15U) return false;
    return !(pin.port == 0U && (pin.pin == 13U || pin.pin == 14U));
}

/*
 * DMA 路由是否合法。
 *
 * controller=0 是“未分配”，在板级表里合法；非零时必须是 DMA1/DMA2，
 * 且 stream/channel 在 0..7。
 *
 * 注意：这里不检查某个外设是否真的支持该 channel——那需要芯片级
 * DMA 请求表，超出本文件职责。
 */
static bool route_ok(board_dma_route_t route)
{
    if (route.controller == 0U) return true;
    return (route.controller == 1U || route.controller == 2U) &&
           route.stream <= 7U && route.channel <= 7U;
}

/* UART 表中是否存在指定实例（用于检查控制台绑定）。 */
static bool uart_present(uint8_t instance)
{
    size_t i;
    for (i = 0U; i < COUNT(s_uarts); ++i) {
        if (s_uarts[i].instance == instance) return true;
    }
    return false;
}

/*
 * 身份与内存布局。
 *
 * 这些断言是“板级契约”：
 *   - 代码区 256K + NVM 256K = 512K 总 Flash；
 *   - NVM 紧跟代码区，因此 stm32f407_nvm.c 用固定偏移就能访问；
 *   - A/B 槽各 128K，正好是一个可独立擦除的 F407 扇区；
 *   - 主 RAM 128K、CCM 64K 与芯片型号一致；
 *   - 控制台实例必须在 UART 表中存在。
 *
 * 如果这些值改变，link.lds、Keil IROM 和 NVM 后端都要同步；
 * tools/tests/test_static_board.py 会交叉检查文本一致性。
 */
static void test_identity_and_memory(void)
{
    CHECK(BOARD_NAME[0] != '\0');
    CHECK(BOARD_VERSION[0] != '\0');
    CHECK(BOARD_FLASH_ORIGIN == 0x08000000U);
    CHECK(BOARD_FLASH_SIZE_KB == 512U);
    CHECK(BOARD_CODE_FLASH_SIZE_KB == 256U);
    CHECK(BOARD_NVM_ORIGIN ==
          BOARD_FLASH_ORIGIN + BOARD_CODE_FLASH_SIZE_KB * 1024U);
    CHECK(BOARD_CODE_FLASH_SIZE_KB + BOARD_NVM_SIZE_KB ==
          BOARD_FLASH_SIZE_KB);
    CHECK(BOARD_NVM_SIZE_KB == 2U * BOARD_NVM_SLOT_KB);
    CHECK(BOARD_RAM_ORIGIN == 0x20000000U);
    CHECK(BOARD_RAM_SIZE_KB == 128U);
    CHECK(BOARD_CCM_ORIGIN == 0x10000000U);
    CHECK(BOARD_CCM_SIZE_KB == 64U);
    CHECK(BOARD_MIN_STACK_SIZE >= 2048U);
    CHECK(BOARD_CONSOLE_UART >= 1U && BOARD_CONSOLE_UART <= 3U);
    CHECK(uart_present(BOARD_CONSOLE_UART));
    CHECK(BOARD_HSE_VALUE == 8000000U);
}

/*
 * 命名引脚（LED/LCD 控制脚）。
 *
 * 这些引脚不是外设复用脚，由 board.c 通过 "pin" 设备配置成输出。
 * 这里锁定默认板的关键位置：
 *   LED=PE2，LCD CS/DC/RES/BL=PA4/PA6/PA3/PA2。
 *
 * LCD DC 与 SPI1 MISO 共用 PA6，这是 SPI1 默认关闭 MISO 的原因；
 * 如果以后把 DC 改到别的脚，可以重新评估 SPI1 全双工。
 */
static void test_named_pins(void)
{
    CHECK(COUNT(s_pins) == BOARD_PIN_COUNT);
    CHECK(s_pins[BOARD_PIN_LED].port == 4U && s_pins[BOARD_PIN_LED].pin == 2U);
    CHECK(s_pins[BOARD_PIN_LCD_CS].port == 0U && s_pins[BOARD_PIN_LCD_CS].pin == 4U);
    CHECK(s_pins[BOARD_PIN_LCD_DC].port == 0U && s_pins[BOARD_PIN_LCD_DC].pin == 6U);
    CHECK(s_pins[BOARD_PIN_LCD_RES].port == 0U && s_pins[BOARD_PIN_LCD_RES].pin == 3U);
    CHECK(s_pins[BOARD_PIN_LCD_BL].port == 0U && s_pins[BOARD_PIN_LCD_BL].pin == 2U);
    CHECK(pin_ok(s_pins[BOARD_PIN_LED]));
    CHECK(pin_ok(s_pins[BOARD_PIN_LCD_CS]));
    CHECK(pin_ok(s_pins[BOARD_PIN_LCD_DC]));
    CHECK(pin_ok(s_pins[BOARD_PIN_LCD_RES]));
    CHECK(pin_ok(s_pins[BOARD_PIN_LCD_BL]));
}

/*
 * UART 表。
 *
 * 重点回归：
 *   - 控制台 UART1 固定 PA9/PA10、AF7；
 *   - UART1 的 DMA 路由必须与已验证的 DMA2S2CH4(RX)/DMA2S7CH4(TX)
 *     一致，profile 打开 DMA 时不会再重新分配；
 *   - 所有表项 TX≠RX、引脚合法、AF 在 4 位范围内、DMA 路由范围合法。
 *
 * 表里提前写好 DMA 路由不代表默认启用：f407ve 仍走中断，
 * f407ve-dma 才通过 Kconfig 打开 UART1 RX/TX DMA。
 */
static void test_uart_table(void)
{
    size_t i;

    CHECK(COUNT(s_uarts) == 3U);
    CHECK(s_uarts[0].instance == 1U);
    CHECK(s_uarts[0].tx.port == 0U && s_uarts[0].tx.pin == 9U);
    CHECK(s_uarts[0].rx.port == 0U && s_uarts[0].rx.pin == 10U);
    CHECK(s_uarts[0].af == 7U);
    CHECK(s_uarts[0].dma_rx.controller == 2U &&
          s_uarts[0].dma_rx.stream == 2U && s_uarts[0].dma_rx.channel == 4U);
    CHECK(s_uarts[0].dma_tx.controller == 2U &&
          s_uarts[0].dma_tx.stream == 7U && s_uarts[0].dma_tx.channel == 4U);

    for (i = 0U; i < COUNT(s_uarts); ++i) {
        const board_uart_t *uart = &s_uarts[i];
        CHECK(uart->instance >= 1U && uart->instance <= 3U);
        CHECK(pin_ok(uart->tx));
        CHECK(pin_ok(uart->rx));
        CHECK(!(uart->tx.port == uart->rx.port &&
                uart->tx.pin == uart->rx.pin));
        CHECK(uart->af <= 15U);
        CHECK(route_ok(uart->dma_rx));
        CHECK(route_ok(uart->dma_tx));
    }
}

/*
 * SPI 表。
 *
 * 重点回归：
 *   - LCD 使用 SPI1：SCK=PA5、MISO=PA6、MOSI=PA7；
 *   - 表里始终填写 MISO，即使默认策略关闭它，这样换策略时无需
 *     改表；是否占用/初始化由 board_policy.h 的 USE_MISO 决定。
 *
 * 与 board.c 的启动校验不同，这里不检查 SPI1 MISO 与 LCD DC
 * 的冲突——那取决于 LCD 是否启用、SPI1 USE_MISO 是否打开，
 * 属于运行时组合校验。
 */
static void test_spi_table(void)
{
    size_t i;

    CHECK(COUNT(s_spis) == 3U);
    CHECK(s_spis[0].instance == 1U);
    CHECK(s_spis[0].sck.port == 0U && s_spis[0].sck.pin == 5U);
    CHECK(s_spis[0].miso.port == 0U && s_spis[0].miso.pin == 6U);
    CHECK(s_spis[0].mosi.port == 0U && s_spis[0].mosi.pin == 7U);

    for (i = 0U; i < COUNT(s_spis); ++i) {
        const board_spi_t *spi = &s_spis[i];
        CHECK(spi->instance >= 1U && spi->instance <= 3U);
        CHECK(pin_ok(spi->sck));
        CHECK(pin_ok(spi->mosi));
        CHECK(pin_ok(spi->miso));
        CHECK(!(spi->sck.port == spi->mosi.port &&
                spi->sck.pin == spi->mosi.pin));
        CHECK(spi->af <= 15U);
        CHECK(route_ok(spi->dma_rx));
        CHECK(route_ok(spi->dma_tx));
    }
}

/*
 * I2C 表。
 *
 * 每个实例 SCL/SDA 必须是不同引脚；I2C2 的 PB10/PB11 与 UART3
 * 冲突，本测试只验证单表结构，组合冲突由 board.c 的启动校验负责。
 */
static void test_i2c_table(void)
{
    size_t i;

    CHECK(COUNT(s_i2cs) == 3U);
    for (i = 0U; i < COUNT(s_i2cs); ++i) {
        const board_i2c_t *i2c = &s_i2cs[i];
        CHECK(i2c->instance >= 1U && i2c->instance <= 3U);
        CHECK(pin_ok(i2c->scl));
        CHECK(pin_ok(i2c->sda));
        CHECK(!(i2c->scl.port == i2c->sda.port &&
                i2c->scl.pin == i2c->sda.pin));
        CHECK(i2c->af <= 15U);
    }
}

/*
 * ADC/PWM 通道表。
 *
 * ADC 重点回归 IN8=PB0、IN9=PB1；PWM 重点回归 TIM4 的 PD12..PD15。
 *
 * 两个循环都检查：
 *   - 通道号在有效范围；
 *   - 同一实例内通道不重复（用位图，避免 O(n^2) 比较）；
 *   - 引脚合法。
 *
 * 模拟引脚与数字外设的冲突由 board.c 的全局 pin_add() 负责；
 * 这里只保证表自身无重复。
 */
static void test_adc_and_pwm_tables(void)
{
    size_t i;
    size_t j;

    CHECK(COUNT(s_adcs) == 1U);
    CHECK(s_adcs[0].instance == 1U);
    CHECK(s_adcs[0].count == 2U);
    CHECK(s_adcs[0].channels[0].channel == 8U);
    CHECK(s_adcs[0].channels[1].channel == 9U);

    for (i = 0U; i < COUNT(s_adcs); ++i) {
        uint16_t mask = 0U;
        CHECK(s_adcs[i].count <= 4U);
        for (j = 0U; j < s_adcs[i].count; ++j) {
            uint8_t channel = s_adcs[i].channels[j].channel;
            CHECK(channel <= 15U);
            CHECK((mask & (1U << channel)) == 0U);
            mask |= (uint16_t)(1U << channel);
            CHECK(pin_ok(s_adcs[i].channels[j].pin));
        }
    }

    CHECK(COUNT(s_pwms) == 1U);
    CHECK(s_pwms[0].instance == 4U);
    CHECK(s_pwms[0].count == 4U);
    for (i = 0U; i < COUNT(s_pwms); ++i) {
        uint8_t mask = 0U;
        CHECK(s_pwms[i].count <= 4U);
        for (j = 0U; j < s_pwms[i].count; ++j) {
            const board_pwm_channel_t *channel = &s_pwms[i].channels[j];
            CHECK(channel->channel >= 1U && channel->channel <= 4U);
            CHECK((mask & (1U << channel->channel)) == 0U);
            mask |= (uint8_t)(1U << channel->channel);
            CHECK(pin_ok(channel->pin));
            CHECK(channel->af <= 15U);
        }
    }
}

/*
 * LCD 面板参数。
 *
 * 只验证结构/范围，不验证时序：
 *   - 使用的 SPI 实例在 1..3；
 *   - 四个命名引脚 ID 不越界；
 *   - 分辨率非零；
 *   - MADCTL=0xC0（当前启动画面的方向）。
 *
 * 如果换面板导致偏移/方向变化，需要同时改 board_config.h 和
 * app/boot 的绘图方向；本测试会拦住 MADCTL 被误改的情况。
 */
static void test_lcd(void)
{
    CHECK(s_lcd.spi_instance >= 1U && s_lcd.spi_instance <= 3U);
    CHECK(s_lcd.cs < BOARD_PIN_COUNT);
    CHECK(s_lcd.dc < BOARD_PIN_COUNT);
    CHECK(s_lcd.res < BOARD_PIN_COUNT);
    CHECK(s_lcd.bl < BOARD_PIN_COUNT);
    CHECK(s_lcd.width > 0U && s_lcd.height > 0U);
    CHECK(s_lcd.madctl == 0xC0U);
}

/*
 * 测试入口；由 test_main.c 调用。
 *
 * 拆成多个小函数而不是一个巨型函数，方便失败时定位到具体表。
 */
void test_board_config_run(void)
{
    test_identity_and_memory();
    test_named_pins();
    test_uart_table();
    test_spi_table();
    test_i2c_table();
    test_adc_and_pwm_tables();
    test_lcd();
}
