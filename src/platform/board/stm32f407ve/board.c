/*
 * board.c - STM32F407VE 板级组合根（mr-library 风格单文件）。
 *
 * 层级：src/platform/board/stm32f407ve
 *
 * 这个文件是整块板子的唯一“组合根”：board_config.h 只描述硬件事实，
 * 这里负责把事实变成可用的设备。
 *
 * 职责：
 *   1. 表转换：board_*_t -> drv_*_hw_t，并按 FW_USING_* 过滤实例；
 *   2. 设备注册：pin/LED/UART/SPI/I2C/ADC/PWM/NVM/LCD；
 *   3. 启动策略：HSI/HSE 时钟、fault 钩子、DMA owner 表、控制台绑定；
 *   4. 启动前校验：端口/保留脚/重复引脚/AF 范围/DMA 路由冲突；
 *   5. 板级角色：console/pin/LED/LCD 查询、板卡信息、复位原因、IWDG。
 *
 * 为什么是单文件：
 *   mr-library 的风格是“一块板一个组合根 + 一组静态表”。之前按设备
 *   类型拆成 board_devices/peripherals/display/storage/clock/pins 多个
 *   文件后，改一个引脚要在多个文件间跳转，换板成本反而更高。现在所有
 *   板级装配都在本文件里按“校验 -> pin -> 外设 -> 存储 -> LCD”顺序
 *   排列，函数之间没有隐藏依赖，也不再有内部头文件。
 *
 * 上下文：
 *   - rt_hw_board_init / board_devices_init：调度器启动前、单线程；
 *   - board_serial_start_all：main 任务中，设备已注册后；
 *   - board_console_dev/board_pin_dev/board_lcd/...：任务上下文；
 *   - board_watchdog_feed：main 的监督循环中。
 *
 * 依赖方向：
 *   board.c -> board_config.h（接线事实）
 *           -> board_policy.h（Kconfig 策略，经 board_config.h 引入）
 *           -> drv_*（芯片驱动）
 *           -> stm32f407_boot/clock/console/fault/iwdg/nvm/panic（平台服务）
 *           -> fw/dev、fw/pin、fw/param、fw/st7735s（框架类）
 *           -> rtthread.h（控制台/组件初始化）
 *   不允许反过来：platform 驱动不包含 board_config.h，也不认识具体板子。
 *
 * 修改指南：
 *   - 改引脚/通道/DMA 路由/LCD/NVM/内存：只改 board_config.h；
 *   - 改时钟源/波特率/IRQ/频率/IWDG：改 Kconfig.board，生成 board_policy.h；
 *   - 新增一个已支持的外设实例：board_config.h 加表行 + Kconfig 打开
 *     FW_USING_xxx，本文件无需改动；
 *   - 新增一个设备类：在 board_config.h 加表/类型，在本文件加
 *     build_xxx_hw() + board_xxx_init()，在 board_devices_init 顺序中
 *     插入，并把驱动源文件登记到 project/sources.json。
 *
 * 失败处理：
 *   所有初始化步骤返回 fw_status_t；board_devices_init 在第一个失败时
 *   打印步骤名并返回，rt_hw_board_init 随后 panic。启动期没有“部分
 *   可用”的降级语义，避免外设半初始化带来的隐蔽故障。
 */
#include "board.h"
#include "board_config.h"
#include "drv_adc.h"
#include "drv_dma.h"
#include "drv_i2c.h"
#include "drv_pin.h"
#include "drv_pwm.h"
#include "drv_serial.h"
#include "drv_spi.h"
#include "fw/param.h"
#include "fw/pin.h"
#include "fw/st7735s.h"
#include "stm32f407_boot.h"
#include "stm32f407_clock.h"
#include "stm32f407_console.h"
#include "stm32f407_fault.h"
#include "stm32f407_iwdg.h"
#include "stm32f407_nvm.h"
#include "stm32f407_panic.h"
#include <rtthread.h>
#include <stddef.h>

/* ====================================================================== */
/* 1. 板级静态表实例                                                     */
/* ====================================================================== */

/*
 * 静态表实例。
 *
 * BOARD_*_TABLE_INIT 在 board_config.h 中定义，这里只负责落地成
 * static const 数组。使用 static const 的原因：
 *   - 放在 Flash（.rodata），不占主 SRAM；
 *   - 链接后不可写，防止运行期误改接线；
 *   - Host 测试直接包含同一份宏，保证测试和固件看到的是同一张表。
 *
 * BOARD_*_COUNT 用 sizeof 推导，新增表行时不需要额外维护计数。
 */
static const board_pin_t s_pins[BOARD_PIN_COUNT] = BOARD_PIN_TABLE_INIT;
static const board_uart_t s_uarts[] = BOARD_UART_TABLE_INIT;
static const board_spi_t s_spis[] = BOARD_SPI_TABLE_INIT;
static const board_i2c_t s_i2cs[] = BOARD_I2C_TABLE_INIT;
static const board_adc_t s_adcs[] = BOARD_ADC_TABLE_INIT;
static const board_pwm_t s_pwms[] = BOARD_PWM_TABLE_INIT;
#if defined(BOARD_LCD_ENABLE) && BOARD_LCD_ENABLE
static const board_lcd_t s_lcd_cfg = BOARD_LCD_INIT;
#endif

#define BOARD_UART_COUNT (sizeof(s_uarts) / sizeof(s_uarts[0]))
#define BOARD_SPI_COUNT  (sizeof(s_spis) / sizeof(s_spis[0]))
#define BOARD_I2C_COUNT  (sizeof(s_i2cs) / sizeof(s_i2cs[0]))
#define BOARD_ADC_COUNT  (sizeof(s_adcs) / sizeof(s_adcs[0]))
#define BOARD_PWM_COUNT  (sizeof(s_pwms) / sizeof(s_pwms[0]))

/*
 * 转换缓冲区。
 *
 * drv_*_register_all() 需要一个连续的 drv_*_hw_t 数组，而板级表里可能
 * 包含 profile 未启用的实例（例如 f407ve 只启用 UART1，但 UART 表里
 * 始终有 3 行）。所以启动时先按 Kconfig 过滤，把启用的表项复制到这些
 * 数组，再把“实际条数”传给驱动。
 *
 * 数组长度用板级表的行数，保证最坏情况下也够用；它们只占 .bss，
 * 每个启动周期只填充一次。
 */
static drv_serial_hw_t s_serial_hw[BOARD_UART_COUNT];
static drv_spi_hw_t s_spi_hw[BOARD_SPI_COUNT];
static drv_i2c_hw_t s_i2c_hw[BOARD_I2C_COUNT];
static drv_adc_hw_t s_adc_hw[BOARD_ADC_COUNT];
static drv_pwm_hw_t s_pwm_hw[BOARD_PWM_COUNT];

#if defined(BOARD_LCD_ENABLE) && BOARD_LCD_ENABLE
static st7735s_t s_lcd;
static bool s_lcd_ready;
#endif

/*
 * 板级初始化是否已经完成。
 *
 * board_devices_init() 是幂等的：第一次成功后置 true，后续调用直接
 * 返回 FW_OK。board_console_dev() 等查询函数也用它区分“尚未初始化”
 * 和“初始化后找不到设备”，避免应用在启动早期拿到半初始化的指针。
 */
static bool s_devices_ready;

/* ====================================================================== */
/* 2. 时钟树                                                             */
/* ====================================================================== */

/*
 * 时钟配置是板级“策略”，不是接线：同一块板可以焊 8MHz 晶振（HSE）
 * 或使用内部 RC（HSI）。Kconfig 的 time choice 生成
 * BOARD_CLOCK_HSE_8_168 / BOARD_CLOCK_HSI_16，这里二选一。
 *
 * stm32f407_clock_cfg_t 字段顺序（见 stm32f407_clock.h）：
 *   use_hse, hse_hz, pll_m, pll_n, pll_p, pll_q,
 *   ahb_div, apb1_div, apb2_div, flash_latency
 * 这里用位置初始化而不是指定初始化，是为了让 GCC/AC5 都能编译，
 * 也便于和结构体定义逐项对照。
 */
#if BOARD_CLOCK_HSE_8_168
#if (BOARD_HSE_VALUE % 1000000U) != 0
#error "BOARD_HSE_VALUE must be an integer multiple of 1MHz"
#endif
#if (BOARD_HSE_VALUE / 1000000U) < 2U || (BOARD_HSE_VALUE / 1000000U) > 63U
#error "BOARD_HSE_VALUE / 1MHz must fit PLLM range 2..63"
#endif

/*
 * HSE 路径：外部 8MHz 晶振 + PLL。
 *
 *   VCO 输入 = HSE / PLLM = 8MHz / 8 = 1MHz
 *   VCO 输出 = 1MHz * PLLN = 336MHz
 *   SYSCLK   = 336MHz / PLLP(2) = 168MHz
 *   USB/SDIO = 336MHz / PLLQ(7) = 48MHz（虽然当前未用，仍给合法值）
 *
 *   AHB  = SYSCLK / 1 = 168MHz（HCLK）
 *   APB1 = 168MHz / 4 = 42MHz（USART2/3、I2C、TIM4 等）
 *   APB2 = 168MHz / 2 = 84MHz（USART1、SPI1 等）
 *   Flash latency = 5（168MHz/3.3V 数据手册要求）
 *
 * 编译期检查 PLLM：
 *   - HSE 必须是 1MHz 的整数倍，否则 PLLM 无法用整数表示；
 *   - PLLM 必须在 2..63，这是 F407 的 PLL 输入分频范围。
 */
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
/*
 * HSI 路径：内部 16MHz RC 直接作为 SYSCLK，不使用 PLL。
 *
 *   SYSCLK = HSI = 16MHz
 *   AHB/APB1/APB2 全部 /1
 *   Flash latency = 0（16MHz 不需要等待周期）
 *
 * use_hse=false 时后面的 PLL 字段和 hse_hz 都不参与初始化；
 * 保留 0 只是让结构体完整、便于阅读。
 */
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

/* ====================================================================== */
/* 3. 实例启用查询                                                       */
/* ====================================================================== */

/*
 * 下面这组函数把 Kconfig 宏翻译成运行期布尔值。
 *
 * 为什么不在 build_*_hw() 里直接写 #if：
 *   - 表遍历需要“跳过未启用项”，用运行期 if 更清晰；
 *   - Kconfig 宏本身是编译期常量，函数会被内联/折叠，开销接近 0；
 *   - 新增实例时只需加一组 #if，不影响表转换逻辑。
 *
 * 驱动侧的静态槽位本身也受 FW_USING_* 保护，因此这里返回 false 的
 * 实例绝不能传给 drv_*_register_all()，否则会访问不存在的槽位。
 */
static bool uart_enabled(uint8_t instance)
{
    (void)instance;
#if defined(FW_USING_UART1) && FW_USING_UART1
    if (instance == 1U) return true;
#endif
#if defined(FW_USING_UART2) && FW_USING_UART2
    if (instance == 2U) return true;
#endif
#if defined(FW_USING_UART3) && FW_USING_UART3
    if (instance == 3U) return true;
#endif
    return false;
}

static bool spi_enabled(uint8_t instance)
{
    (void)instance;
#if defined(FW_USING_SPI1) && FW_USING_SPI1
    if (instance == 1U) return true;
#endif
#if defined(FW_USING_SPI2) && FW_USING_SPI2
    if (instance == 2U) return true;
#endif
#if defined(FW_USING_SPI3) && FW_USING_SPI3
    if (instance == 3U) return true;
#endif
    return false;
}

static bool i2c_enabled(uint8_t instance)
{
    (void)instance;
#if defined(FW_USING_I2C1) && FW_USING_I2C1
    if (instance == 1U) return true;
#endif
#if defined(FW_USING_I2C2) && FW_USING_I2C2
    if (instance == 2U) return true;
#endif
#if defined(FW_USING_I2C3) && FW_USING_I2C3
    if (instance == 3U) return true;
#endif
    return false;
}

static bool adc_enabled(uint8_t instance)
{
    (void)instance;
#if defined(FW_USING_ADC1) && FW_USING_ADC1
    if (instance == 1U) return true;
#endif
    return false;
}

static bool pwm_enabled(uint8_t instance)
{
    (void)instance;
#if defined(FW_USING_PWM4) && FW_USING_PWM4
    if (instance == 4U) return true;
#endif
    return false;
}

/*
 * UART/SPI 的 DMA 是否启用。
 *
 * 这是“两层开关”：
 *   1. Kconfig 的 FW_USING_UARTn_DMA_RX/TX、FW_USING_SPIn_DMA 决定
 *      profile 是否允许该方向走 DMA；
 *   2. board_config.h 的路由 controller != 0 决定硬件上是否分配了
 *      stream/channel。
 * 两者同时满足才算启用，避免“Kconfig 打开了但板级没接线”时把
 * 0/0/0 当成合法路由传给 drv_dma_claim。
 */
static bool uart_dma_rx_enabled(uint8_t instance)
{
    (void)instance;
#if defined(FW_USING_UART1_DMA_RX) && FW_USING_UART1_DMA_RX
    if (instance == 1U) return true;
#endif
#if defined(FW_USING_UART2_DMA_RX) && FW_USING_UART2_DMA_RX
    if (instance == 2U) return true;
#endif
#if defined(FW_USING_UART3_DMA_RX) && FW_USING_UART3_DMA_RX
    if (instance == 3U) return true;
#endif
    return false;
}

static bool uart_dma_tx_enabled(uint8_t instance)
{
    (void)instance;
#if defined(FW_USING_UART1_DMA_TX) && FW_USING_UART1_DMA_TX
    if (instance == 1U) return true;
#endif
#if defined(FW_USING_UART2_DMA_TX) && FW_USING_UART2_DMA_TX
    if (instance == 2U) return true;
#endif
#if defined(FW_USING_UART3_DMA_TX) && FW_USING_UART3_DMA_TX
    if (instance == 3U) return true;
#endif
    return false;
}

static bool spi_dma_enabled(uint8_t instance)
{
    (void)instance;
#if defined(FW_USING_SPI1_DMA) && FW_USING_SPI1_DMA
    if (instance == 1U) return true;
#endif
#if defined(FW_USING_SPI2_DMA) && FW_USING_SPI2_DMA
    if (instance == 2U) return true;
#endif
#if defined(FW_USING_SPI3_DMA) && FW_USING_SPI3_DMA
    if (instance == 3U) return true;
#endif
    return false;
}

/*
 * 逐实例读取 SPI 策略。
 *
 * 这些值来自 Kconfig（board_policy.h），与引脚表分开：
 *   use_miso 决定驱动初始化成 2LINES 还是 1LINE；
 *   max_hz   决定 SCK 分频上限。
 * 实例号不在 1..3 时返回安全值，调用方会在校验阶段拒绝。
 */
static bool spi_use_miso(uint8_t instance)
{
    switch (instance) {
    case 1U: return BOARD_SPI1_USE_MISO != 0;
    case 2U: return BOARD_SPI2_USE_MISO != 0;
    case 3U: return BOARD_SPI3_USE_MISO != 0;
    default: return false;
    }
}

static uint32_t spi_max_hz(uint8_t instance)
{
    switch (instance) {
    case 1U: return BOARD_SPI1_MAX_HZ;
    case 2U: return BOARD_SPI2_MAX_HZ;
    case 3U: return BOARD_SPI3_MAX_HZ;
    default: return 0U;
    }
}

static uint32_t i2c_speed_hz(uint8_t instance)
{
    switch (instance) {
    case 1U: return BOARD_I2C1_SPEED_HZ;
    case 2U: return BOARD_I2C2_SPEED_HZ;
    case 3U: return BOARD_I2C3_SPEED_HZ;
    default: return 0U;
    }
}

/*
 * 把板级 DMA 路由转换成驱动路由。
 *
 * board_config.h 用 controller=0 表示“没有分配”，而驱动用 0xFF
 * （DRV_DMA_NONE）作为哨兵。这里做唯一一次转换，保证：
 *   - 未分配的路由三个字段都是 0xFF，不会被误认为 DMA1 Stream0；
 *   - 已分配的路由原样传递，范围由 board_pins_validate() 检查。
 */
static drv_dma_route_t to_route(board_dma_route_t route)
{
    drv_dma_route_t result;
    if (route.controller == 0U) {
        result.controller = DRV_DMA_NONE;
        result.stream = DRV_DMA_NONE;
        result.channel = DRV_DMA_NONE;
    } else {
        result.controller = route.controller;
        result.stream = route.stream;
        result.channel = route.channel;
    }
    return result;
}

/* ====================================================================== */
/* 4. 表 -> 驱动描述符转换                                               */
/* ====================================================================== */

/*
 * 转换函数共同约定：
 *   - 输入是 board_config.h 的 static const 表，不修改原表；
 *   - 输出是 s_*_hw[] 转换缓冲区，返回值是实际写入的条目数；
 *   - 未启用的实例直接跳过（由 *_enabled() 判断）；
 *   - 策略字段（波特率、IRQ、频率、MISO、电气参数）从 board_policy.h
 *     取值，不重复写在表里；
 *   - 只做“格式转换”，不做硬件访问；失败检查由 board_pins_validate()
 *     和驱动注册函数负责。
 *
 * 这样 board_config.h 只描述“接了什么”，board.c 只描述“怎么交给驱动”，
 * 两者职责清晰，新增实例时不需要复制大段初始化代码。
 */

/*
 * UART 表 -> drv_serial_hw_t。
 *
 * 每个启用的实例产生一条描述符：
 *   instance/tx/rx/af 来自板级表；
 *   baud/irq_priority 来自策略；
 *   dma_rx/dma_tx 先转换哨兵，再根据 Kconfig + 路由是否存在决定
 *   *_enabled。驱动看到 enabled=false 时走 RXNE/TXE 中断路径。
 *
 * 返回实际条目数；0 表示当前 profile 没有启用任何 UART。
 */
static size_t build_serial_hw(void)
{
    size_t i;
    size_t count = 0U;

    for (i = 0U; i < BOARD_UART_COUNT; ++i) {
        const board_uart_t *src = &s_uarts[i];
        drv_serial_hw_t *hw;

        if (!uart_enabled(src->instance)) continue;
        hw = &s_serial_hw[count++];
        hw->instance = src->instance;
        hw->tx_port = src->tx.port;
        hw->tx_pin = src->tx.pin;
        hw->tx_af = src->af;
        hw->rx_port = src->rx.port;
        hw->rx_pin = src->rx.pin;
        hw->rx_af = src->af;
        hw->baud = BOARD_UART_BAUD;
        hw->irq_priority = (uint8_t)BOARD_UART_IRQ_PRIORITY;
        hw->dma_rx = to_route(src->dma_rx);
        hw->dma_tx = to_route(src->dma_tx);
        hw->dma_rx_enabled = uart_dma_rx_enabled(src->instance) &&
                             src->dma_rx.controller != 0U;
        hw->dma_tx_enabled = uart_dma_tx_enabled(src->instance) &&
                             src->dma_tx.controller != 0U;
    }
    return count;
}

/*
 * SPI 表 -> drv_spi_hw_t。
 *
 * 与 UART 的区别：
 *   - 三根信号线共用同一个 AF，转换时分别填入 sck/mosi/miso；
 *   - use_miso 来自策略，决定是否把 MISO 引脚配置为复用；
 *   - dma_rx_enabled 额外要求 dma_tx_enabled && use_miso：
 *     HAL 在 2LINES master 模式下接收需要 TX DMA 句柄存在，且没有
 *     MISO 就没有接收通道；
 *   - max_hz 来自策略，供驱动计算 SCK 分频。
 *
 * 返回实际条目数；0 表示当前 profile 没有启用 SPI。
 */
static size_t build_spi_hw(void)
{
    size_t i;
    size_t count = 0U;

    for (i = 0U; i < BOARD_SPI_COUNT; ++i) {
        const board_spi_t *src = &s_spis[i];
        drv_spi_hw_t *hw;
        bool use_miso;

        if (!spi_enabled(src->instance)) continue;
        use_miso = spi_use_miso(src->instance);
        hw = &s_spi_hw[count++];
        hw->instance = src->instance;
        hw->sck_port = src->sck.port;
        hw->sck_pin = src->sck.pin;
        hw->sck_af = src->af;
        hw->mosi_port = src->mosi.port;
        hw->mosi_pin = src->mosi.pin;
        hw->mosi_af = src->af;
        hw->miso_port = src->miso.port;
        hw->miso_pin = src->miso.pin;
        hw->miso_af = src->af;
        hw->use_miso = use_miso;
        hw->max_hz = spi_max_hz(src->instance);
        hw->dma_rx = to_route(src->dma_rx);
        hw->dma_tx = to_route(src->dma_tx);
        hw->dma_tx_enabled = spi_dma_enabled(src->instance) &&
                             src->dma_tx.controller != 0U;
        hw->dma_rx_enabled = hw->dma_tx_enabled && use_miso &&
                             src->dma_rx.controller != 0U;
    }
    return count;
}

/*
 * I2C 表 -> drv_i2c_hw_t。
 *
 * 只转换 SCL/SDA 引脚、AF 和总线频率；I2C 不使用 DMA/中断，
 * 驱动全部是主模式阻塞事务。
 *
 * 返回实际条目数；0 表示当前 profile 没有启用 I2C。
 */
static size_t build_i2c_hw(void)
{
    size_t i;
    size_t count = 0U;

    for (i = 0U; i < BOARD_I2C_COUNT; ++i) {
        const board_i2c_t *src = &s_i2cs[i];
        drv_i2c_hw_t *hw;

        if (!i2c_enabled(src->instance)) continue;
        hw = &s_i2c_hw[count++];
        hw->instance = src->instance;
        hw->scl_port = src->scl.port;
        hw->scl_pin = src->scl.pin;
        hw->scl_af = src->af;
        hw->sda_port = src->sda.port;
        hw->sda_pin = src->sda.pin;
        hw->sda_af = src->af;
        hw->clock_hz = i2c_speed_hz(src->instance);
    }
    return count;
}

/*
 * ADC 表 -> drv_adc_hw_t。
 *
 *   channel_count 来自板级表的 count；
 *   每个通道复制 channel/port/pin（模拟引脚没有 AF）；
 *   vref/resolution/sample_cycles 来自策略。
 *
 * 内层循环同时受 src->count 和 DRV_ADC_MAX_CHANNELS 限制，即使表里
 * 写了过多通道也不会越界；正常配置下 board_pins_validate() 会先拒绝
 * 超限的表。
 *
 * 返回实际条目数；0 表示当前 profile 没有启用 ADC。
 */
static size_t build_adc_hw(void)
{
    size_t i;
    size_t j;
    size_t count = 0U;

    for (i = 0U; i < BOARD_ADC_COUNT; ++i) {
        const board_adc_t *src = &s_adcs[i];
        drv_adc_hw_t *hw;

        if (!adc_enabled(src->instance)) continue;
        hw = &s_adc_hw[count++];
        hw->instance = src->instance;
        hw->channel_count = src->count;
        for (j = 0U; j < src->count && j < DRV_ADC_MAX_CHANNELS; ++j) {
            hw->channels[j].channel = src->channels[j].channel;
            hw->channels[j].port = src->channels[j].pin.port;
            hw->channels[j].pin = src->channels[j].pin.pin;
        }
        hw->vref_mv = (uint16_t)BOARD_ADC1_VREF_MV;
        hw->resolution_bits = (uint8_t)BOARD_ADC1_RESOLUTION_BITS;
        hw->sample_cycles = (uint16_t)BOARD_ADC1_SAMPLE_CYCLES;
    }
    return count;
}

/*
 * PWM 表 -> drv_pwm_hw_t。
 *
 *   channel_count 来自板级表的 count；
 *   每个通道复制 channel/port/pin/af；
 *   frequency_hz 来自策略 BOARD_PWM4_PERIOD_HZ。
 *
 * 驱动首版只支持 TIM4，因此实例号固定为 4；内层循环同样受
 * DRV_PWM_MAX_CHANNELS 保护。
 *
 * 返回实际条目数；0 表示当前 profile 没有启用 PWM。
 */
static size_t build_pwm_hw(void)
{
    size_t i;
    size_t j;
    size_t count = 0U;

    for (i = 0U; i < BOARD_PWM_COUNT; ++i) {
        const board_pwm_t *src = &s_pwms[i];
        drv_pwm_hw_t *hw;

        if (!pwm_enabled(src->instance)) continue;
        hw = &s_pwm_hw[count++];
        hw->instance = src->instance;
        hw->channel_count = src->count;
        for (j = 0U; j < src->count && j < DRV_PWM_MAX_CHANNELS; ++j) {
            hw->channels[j].channel = src->channels[j].channel;
            hw->channels[j].port = src->channels[j].pin.port;
            hw->channels[j].pin = src->channels[j].pin.pin;
            hw->channels[j].af = src->channels[j].af;
        }
        hw->frequency_hz = BOARD_PWM4_PERIOD_HZ;
    }
    return count;
}

/* ====================================================================== */
/* 5. 启动前接线校验                                                     */
/* ====================================================================== */

/*
 * 校验阶段只使用两个小型的“占用表”：
 *
 *   board_pin_use_t - 已占用的 GPIO，用于发现重复引脚；
 *   board_dma_use_t - 已占用的 (controller, stream)，用于发现 DMA 冲突。
 *
 * 容量是编译期常量，不是动态分配：
 *   - 当前全配置约 32 个 GPIO，48 留出余量；
 *   - F407 只有 DMA1/DMA2 共 16 条 stream，按 (controller,stream)
 *     去重后最多 16 条；这里只记录板级实际启用的 DMA，8 条足够
 *     当前 UART/SPI 的上限。
 * 如果以后板级表超过容量，校验会返回 FW_EINVAL，而不是越界写入。
 */
typedef struct {
    uint8_t port; /* 端口 0..4 */
    uint8_t pin;  /* 引脚 0..15 */
} board_pin_use_t;

typedef struct {
    uint8_t controller; /* 1=DMA1, 2=DMA2 */
    uint8_t stream;     /* 0..7 */
} board_dma_use_t;

#define BOARD_PIN_CAPACITY 48U
#define BOARD_DMA_CAPACITY 8U

/*
 * 单个 GPIO 是否可以作为板级功能脚。
 *
 * 规则：
 *   - F407VE 100 脚封装只有 GPIOA..GPIOE，port 必须 <= 4；
 *   - 每个端口 16 个引脚，pin 必须 <= 15；
 *   - PA13/PA14 固定给 SWDIO/SWCLK，任何板级表都不能占用，
 *     否则下载/调试会失效。
 *
 * 这个检查不判断 AF 是否属于某个外设，也不判断电气特性；
 * 这些由数据手册和上板验证保证。
 */
static bool pin_valid(uint8_t port, uint8_t pin)
{
    if (port > 4U || pin > 15U) return false;
    /* PA13/PA14 是 SWDIO/SWCLK，板级接线不能占用。 */
    return !(port == 0U && (pin == 13U || pin == 14U));
}

/*
 * 登记一个 GPIO。
 *
 * 返回 false 表示：
 *   - 引脚本身非法（越界或 SWD 保留）；
 *   - 已经被其他功能占用（重复引脚）；
 *   - 占用表已满。
 *
 * 调用者只需把 false 转成 FW_EINVAL；具体原因由启动日志/调试器排查。
 * 之所以不在函数里打印具体端口，是为了让校验函数保持无 OS 依赖、
 * 可在任何上下文调用。
 */
static bool pin_add(board_pin_use_t *used, size_t *count, uint8_t port,
                    uint8_t pin)
{
    size_t i;

    if (!pin_valid(port, pin) || *count >= BOARD_PIN_CAPACITY) return false;
    for (i = 0U; i < *count; ++i) {
        if (used[i].port == port && used[i].pin == pin) return false;
    }
    used[*count].port = port;
    used[*count].pin = pin;
    ++(*count);
    return true;
}

/*
 * DMA 路由是否合法。
 *
 * controller 必须是 1/2（0 表示未分配，调用方不会传到这里）；
 * stream/channel 必须在 0..7。这里不检查“该外设是否真的支持这个
 * channel”，因为那需要芯片级 DMA 请求表；board_config.h 的注释
 * 和数据手册负责正确性。
 */
static bool dma_route_valid(board_dma_route_t route)
{
    return (route.controller == 1U || route.controller == 2U) &&
           route.stream <= 7U && route.channel <= 7U;
}

/*
 * 登记一条 DMA 路由。
 *
 * 同一个 (controller, stream) 只能被一个方向占用；F4 的 DMA 每个
 * stream 同时只能服务一个请求。channel 不同但 stream 相同也算冲突。
 *
 * 返回 false 表示路由非法、已被占用或占用表已满。
 */
static bool dma_add(board_dma_use_t *used, size_t *count,
                    board_dma_route_t route)
{
    size_t i;

    if (!dma_route_valid(route) || *count >= BOARD_DMA_CAPACITY) return false;
    for (i = 0U; i < *count; ++i) {
        if (used[i].controller == route.controller &&
            used[i].stream == route.stream) return false;
    }
    used[*count].controller = route.controller;
    used[*count].stream = route.stream;
    ++(*count);
    return true;
}

/*
 * 校验所有启用实例的接线。
 *
 * 执行顺序与依赖：
 *   1. 命名引脚（LED/LCD 控制脚）范围：即使功能未启用也必须是合法 GPIO；
 *   2. UART：TX≠RX、AF 范围、重复引脚、启用方向的 DMA 路由；
 *   3. SPI：SCK/MOSI（以及启用时的 MISO）、AF、DMA；
 *      RX DMA 要求 MISO 与 TX 路由同时存在；
 *   4. I2C：SCL≠SDA、AF、重复引脚；
 *   5. ADC：通道 0..15 且同一实例内唯一、模拟引脚不重复；
 *   6. PWM：通道 1..4 且同一实例内唯一、AF、引脚不重复；
 *   7. LED/LCD：如果启用，控制脚不能与外设复用脚冲突；
 *      LCD 使用的 SPI 实例必须已启用。
 *
 * 返回 FW_EINVAL 表示接线结构非法；调用方 board_devices_init 会
 * 打印步骤名并让启动 panic。这里不做“自动修正”，因为板级表是
 * 硬件事实，自动修正只会掩盖接线错误。
 *
 * AF 正确性不在本函数校验：同一实例的 AF 已经由表结构保证一致，
 * 但“这个 AF 是否真的对应这个外设”需要数据手册/上板验证；
 * Host 测试覆盖结构和关键引脚回归。
 */
static fw_status_t board_pins_validate(void)
{
    board_pin_use_t used[BOARD_PIN_CAPACITY];
    board_dma_use_t dma_used[BOARD_DMA_CAPACITY];
    size_t used_count = 0U;
    size_t dma_count = 0U;
    size_t i;
    size_t j;

    /* 命名引脚（LED/LCD 控制脚）即使未启用也必须落在合法 GPIO 上。 */
    for (i = 0U; i < BOARD_PIN_COUNT; ++i) {
        if (!pin_valid(s_pins[i].port, s_pins[i].pin)) return FW_EINVAL;
    }

    /*
     * UART：
     *   - AF7 是 USART1..3 的常用复用号，这里只检查 4 位范围；
     *   - TX/RX 不能是同一根线（否则发送会回环到接收）；
     *   - 只有 Kconfig 启用且路由非零的方向才登记 DMA。
     */
    for (i = 0U; i < BOARD_UART_COUNT; ++i) {
        const board_uart_t *uart = &s_uarts[i];

        if (!uart_enabled(uart->instance)) continue;
        if (uart->af > 15U) return FW_EINVAL;
        if (uart->tx.port == uart->rx.port &&
            uart->tx.pin == uart->rx.pin) return FW_EINVAL;
        if (!pin_add(used, &used_count, uart->tx.port, uart->tx.pin) ||
            !pin_add(used, &used_count, uart->rx.port, uart->rx.pin))
            return FW_EINVAL;
        if (uart_dma_rx_enabled(uart->instance) &&
            uart->dma_rx.controller != 0U &&
            !dma_add(dma_used, &dma_count, uart->dma_rx))
            return FW_EINVAL;
        if (uart_dma_tx_enabled(uart->instance) &&
            uart->dma_tx.controller != 0U &&
            !dma_add(dma_used, &dma_count, uart->dma_tx))
            return FW_EINVAL;
    }

    /*
     * SPI：
     *   - SCK/MOSI 总是占用；MISO 只在策略启用时占用；
     *   - SPI1 的 PA6 同时是 LCD DC，因此默认 USE_MISO=0 时不会冲突；
     *   - DMA 部分与 build_spi_hw() 的启用条件保持一致，避免“转换时
     *     启用、校验时漏检”或反之。
     */
    for (i = 0U; i < BOARD_SPI_COUNT; ++i) {
        const board_spi_t *spi = &s_spis[i];
        bool use_miso = spi_use_miso(spi->instance);

        if (!spi_enabled(spi->instance)) continue;
        if (spi->af > 15U) return FW_EINVAL;
        if (!pin_add(used, &used_count, spi->sck.port, spi->sck.pin) ||
            !pin_add(used, &used_count, spi->mosi.port, spi->mosi.pin))
            return FW_EINVAL;
        if (use_miso &&
            !pin_add(used, &used_count, spi->miso.port, spi->miso.pin))
            return FW_EINVAL;
        if (!spi_dma_enabled(spi->instance)) continue;
        if (spi->dma_tx.controller != 0U &&
            !dma_add(dma_used, &dma_count, spi->dma_tx))
            return FW_EINVAL;
        /*
         * HAL 在 2LINES master 下接收需要 TX 方向也已配置；
         * 没有 MISO 或没有 TX 路由时不能启用 RX DMA。
         */
        if (spi->dma_rx.controller != 0U) {
            if (!use_miso || spi->dma_tx.controller == 0U)
                return FW_EINVAL;
            if (!dma_add(dma_used, &dma_count, spi->dma_rx))
                return FW_EINVAL;
        }
    }

    /*
     * I2C：
     *   - SCL/SDA 必须是两根不同的线；
     *   - 引脚登记会顺带发现 I2C2(PB10/PB11) 与 UART3 的冲突。
     */
    for (i = 0U; i < BOARD_I2C_COUNT; ++i) {
        const board_i2c_t *i2c = &s_i2cs[i];

        if (!i2c_enabled(i2c->instance)) continue;
        if (i2c->af > 15U) return FW_EINVAL;
        if (i2c->scl.port == i2c->sda.port &&
            i2c->scl.pin == i2c->sda.pin) return FW_EINVAL;
        if (!pin_add(used, &used_count, i2c->scl.port, i2c->scl.pin) ||
            !pin_add(used, &used_count, i2c->sda.port, i2c->sda.pin))
            return FW_EINVAL;
    }

    /*
     * ADC：
     *   - 通道号 0..15；
     *   - 同一实例内不能重复配置同一通道（否则 drv_adc 的 channel_mask
     *     会掩盖重复）；
     *   - 模拟引脚同样加入全局占用表，不能与数字外设共用。
     */
    for (i = 0U; i < BOARD_ADC_COUNT; ++i) {
        const board_adc_t *adc = &s_adcs[i];
        uint16_t channel_mask = 0U;

        if (!adc_enabled(adc->instance)) continue;
        if (adc->count > DRV_ADC_MAX_CHANNELS) return FW_EINVAL;
        for (j = 0U; j < adc->count; ++j) {
            uint8_t channel = adc->channels[j].channel;
            if (channel > 15U ||
                (channel_mask & (1U << channel)) != 0U)
                return FW_EINVAL;
            channel_mask |= (uint16_t)(1U << channel);
            if (!pin_add(used, &used_count, adc->channels[j].pin.port,
                         adc->channels[j].pin.pin))
                return FW_EINVAL;
        }
    }

    /*
     * PWM：
     *   - 定时器通道号 1..4；
     *   - 同一实例内通道唯一；
     *   - 引脚和 AF 范围检查与 UART/SPI/I2C 相同。
     */
    for (i = 0U; i < BOARD_PWM_COUNT; ++i) {
        const board_pwm_t *pwm = &s_pwms[i];
        uint8_t channel_mask = 0U;

        if (!pwm_enabled(pwm->instance)) continue;
        if (pwm->count > DRV_PWM_MAX_CHANNELS) return FW_EINVAL;
        for (j = 0U; j < pwm->count; ++j) {
            const board_pwm_channel_t *channel = &pwm->channels[j];
            if (channel->channel < 1U || channel->channel > 4U ||
                (channel_mask & (1U << channel->channel)) != 0U)
                return FW_EINVAL;
            channel_mask |= (uint8_t)(1U << channel->channel);
            if (channel->af > 15U) return FW_EINVAL;
            if (!pin_add(used, &used_count, channel->pin.port,
                         channel->pin.pin))
                return FW_EINVAL;
        }
    }

    /*
     * LED：只有启用时才占用引脚；如果与某个外设复用脚冲突，
     * 说明板级表写错，直接拒绝启动。
     */
#if defined(BOARD_LED_ENABLE) && BOARD_LED_ENABLE
    if (!pin_add(used, &used_count, s_pins[BOARD_PIN_LED].port,
                 s_pins[BOARD_PIN_LED].pin))
        return FW_EINVAL;
#endif

    /*
     * LCD：
     *   - 使用的 SPI 实例必须已启用；
     *   - CS/DC/RES/BL 四个控制脚必须合法且不与外设冲突；
     *   - 默认 SPI1 关闭 MISO，因此 DC(PA6) 不会被 SPI1 MISO 占用。
     */
#if defined(BOARD_LCD_ENABLE) && BOARD_LCD_ENABLE
    if (!spi_enabled(s_lcd_cfg.spi_instance)) return FW_EINVAL;
    for (i = 0U; i < 4U; ++i) {
        board_pin_id_t id = (i == 0U) ? s_lcd_cfg.cs
                           : (i == 1U) ? s_lcd_cfg.dc
                           : (i == 2U) ? s_lcd_cfg.res
                                       : s_lcd_cfg.bl;
        if (id >= BOARD_PIN_COUNT) return FW_EINVAL;
        if (!pin_add(used, &used_count, s_pins[id].port, s_pins[id].pin))
            return FW_EINVAL;
    }
#endif

    return FW_OK;
}

/* ====================================================================== */
/* 6. 初始化步骤                                                         */
/* ====================================================================== */

/*
 * 平台级初始化。
 *
 * 必须在任何设备注册之前完成：
 *   - stm32f407_fault_init()：打开 MemManage/BusFault/UsageFault，
 *     并给 RT-Thread 安装 HardFault 钩子；之后任何初始化错误都能
 *     输出现场。
 *   - drv_dma_init()：清空 DMA stream 的 owner 表。UART/SPI 注册时
 *     会按需 claim，若这里不清零，重复初始化可能残留旧 owner。
 */
static fw_status_t board_system_init(void)
{
    stm32f407_fault_init();
    drv_dma_init();
    return FW_OK;
}

/*
 * 注册唯一的 "pin" 设备。
 *
 * 后续 LED、LCD 控制脚、所有外设 GPIO 配置都通过这个设备完成，
 * 所以它是第一个真正访问 GPIO 的步骤。驱动内部只注册一次；
 * 重复调用由 board_devices_init 的 s_devices_ready 保护。
 */
static fw_status_t board_pin_init(void)
{
    return drv_pin_register();
}

/*
 * 配置板载 LED 为推挽输出，并写入“熄灭”电平。
 *
 * BOARD_LED_ACTIVE_HIGH=1 时写 0 熄灭；=0 时写 1 熄灭。
 * LED 未启用时直接返回 FW_OK，不参与后续启动。
 */
static fw_status_t board_led_init(void)
{
#if defined(BOARD_LED_ENABLE) && BOARD_LED_ENABLE
    fw_dev_t *pin = fw_dev_find("pin");
    int led = FW_PIN_NUMBER(s_pins[BOARD_PIN_LED].port,
                            s_pins[BOARD_PIN_LED].pin);

    if (pin == NULL) return FW_ENOTREADY;
    if (fw_pin_configure(pin, led, FW_PIN_MODE_OUTPUT) < 0) return FW_EIO;
    (void)fw_pin_write(pin, led, BOARD_LED_ACTIVE_HIGH ? 0 : 1);
#endif
    return FW_OK;
}

/*
 * 注册 UART 并绑定控制台。
 *
 * 流程：
 *   1. build_serial_hw() 按 Kconfig 过滤 s_uarts[]，填充 s_serial_hw[]；
 *   2. drv_serial_register_all() 为每个实例配置 GPIO、HAL、ring 和
 *      设备名 serialN，但还不打开 NVIC；
 *   3. 找到 BOARD_CONSOLE_UART 对应的 drv_serial_t，绑定到
 *      stm32f407_console_bind()。
 *
 * 没有启用任何 UART 时返回 FW_ENOTREADY：本框架默认要求控制台串口
 * 存在，否则 panic/fault 现场无法输出。控制台实例不在已注册列表中
 * 同样视为配置错误。
 *
 * 注意：中断打开是 main 里 board_serial_start_all() 的职责，这里
 * 只完成设备注册，避免启动早期 ISR 在设备未就绪时触发。
 */
static fw_status_t board_serial_init(void)
{
    size_t count = build_serial_hw();
    drv_serial_t *console;
    fw_status_t result;

    if (count == 0U) return FW_ENOTREADY;
    result = drv_serial_register_all(s_serial_hw, count);
    if (result != FW_OK) return result;

    console = drv_serial_instance(BOARD_CONSOLE_UART);
    if (console == NULL) return FW_ENOTREADY;
    stm32f407_console_bind(console);
    return FW_OK;
}

/*
 * 注册 SPI 设备。
 *
 * build 返回 0 表示当前 profile 没有启用任何 SPI，直接成功返回；
 * 这样关闭 SPI 的 profile 不需要额外的 #if 包裹整个步骤。
 *
 * 驱动内部按 FW_USING_SPIn 保留静态槽位；LCD 依赖 spiN 设备名，
 * 因此 SPI 必须在 display 之前注册。
 */
static fw_status_t board_spi_init(void)
{
    size_t count = build_spi_hw();
    if (count == 0U) return FW_OK;
    return drv_spi_register_all(s_spi_hw, count);
}

/*
 * 注册 I2C 设备（阻塞主模式，无中断/DMA）。
 */
static fw_status_t board_i2c_init(void)
{
    size_t count = build_i2c_hw();
    if (count == 0U) return FW_OK;
    return drv_i2c_register_all(s_i2c_hw, count);
}

/*
 * 注册 ADC 设备（单通道、单次转换、轮询）。
 */
static fw_status_t board_adc_init(void)
{
    size_t count = build_adc_hw();
    if (count == 0U) return FW_OK;
    return drv_adc_register_all(s_adc_hw, count);
}

/*
 * 注册 PWM 设备（TIM4 四路，初始化后不启动输出）。
 */
static fw_status_t board_pwm_init(void)
{
    size_t count = build_pwm_hw();
    if (count == 0U) return FW_OK;
    return drv_pwm_register_all(s_pwm_hw, count);
}

/*
 * 初始化 NVM 后端并扫描 A/B 参数槽。
 *
 * 两个条件同时满足才执行：
 *   - BOARD_NVM_ENABLED：板级确实有 NVM 分区；
 *   - FW_USE_PARAM：框架包含参数存储组件。
 *
 * stm32f407_nvm_init() 只准备 Flash 后端，fw_param_init() 才会读取
 * 两个槽的 CRC 并选择最新有效槽。失败通常意味着 Flash 擦除/编程
 * 不可用，属于致命错误，向上返回让启动 panic。
 */
static fw_status_t board_storage_init(void)
{
#if defined(BOARD_NVM_ENABLED) && BOARD_NVM_ENABLED && \
    defined(FW_USE_PARAM) && FW_USE_PARAM
    fw_status_t result = stm32f407_nvm_init();
    if (result != FW_OK) return result;
    return fw_param_init(stm32f407_nvm_backend(), FW_PARAM_DATA_MAX);
#else
    return FW_OK;
#endif
}

#if defined(BOARD_LCD_ENABLE) && BOARD_LCD_ENABLE
/*
 * 把命名引脚 ID 转成 pin 设备使用的 FW_PIN_NUMBER 编码。
 *
 * s_pins[] 的下标已经由 board_pin_id_t 保证合法（校验阶段也检查过）；
 * 这里不再做边界判断，避免在启动路径重复分支。
 */
static int lcd_pin(board_pin_id_t id)
{
    return FW_PIN_NUMBER(s_pins[id].port, s_pins[id].pin);
}

/*
 * 按 s_lcd_cfg.spi_instance 查找 SPI 设备。
 *
 * 只接受 spi1..spi3；返回 NULL 表示实例号非法或该 profile 没有启用
 * 对应 SPI，调用方会返回 FW_ENOTREADY。
 */
static fw_dev_t *lcd_spi_dev(void)
{
    switch (s_lcd_cfg.spi_instance) {
    case 1U: return fw_dev_find("spi1");
    case 2U: return fw_dev_find("spi2");
    case 3U: return fw_dev_find("spi3");
    default: return NULL;
    }
}

/*
 * 配置 LCD 的四个控制脚。
 *
 * 初始电平按 ST7735S 的约定：
 *   CS=1   不选中（避免上电时误收命令）；
 *   DC=1   数据模式（st7735s_init 会自己切换命令/数据）；
 *   RES=1  不复位（释放硬件复位）；
 *   BL=熄灭 按有效电平取反，背光等显示初始化完成后再打开。
 *
 * 任一脚配置失败返回 FW_EIO；启动阶段没有 GPIO 可用的降级方案。
 */
static fw_status_t lcd_pins_init(fw_dev_t *pin)
{
    const int pins[4] = {
        lcd_pin(s_lcd_cfg.cs), lcd_pin(s_lcd_cfg.dc),
        lcd_pin(s_lcd_cfg.res), lcd_pin(s_lcd_cfg.bl)
    };
    size_t i;

    for (i = 0U; i < 4U; ++i) {
        if (fw_pin_configure(pin, pins[i], FW_PIN_MODE_OUTPUT) < 0)
            return FW_EIO;
    }
    (void)fw_pin_write(pin, pins[0], 1);
    (void)fw_pin_write(pin, pins[1], 1);
    (void)fw_pin_write(pin, pins[2], 1);
    (void)fw_pin_write(pin, pins[3], s_lcd_cfg.bl_active_high ? 0 : 1);
    return FW_OK;
}
#endif

/*
 * 注册 "lcd" 显示设备。
 *
 * 依赖：
 *   - "pin" 设备（配置 CS/DC/RES/BL）；
 *   - 已注册的 spiN（s_lcd_cfg.spi_instance）。
 *
 * 步骤：
 *   1. 配置四个控制脚为输出并置为安全初始电平；
 *   2. 把板级面板参数复制到 st7735s_config_t；
 *   3. st7735s_init() 发送面板初始化序列并注册 "lcd" 设备；
 *   4. 标记 s_lcd_ready，供 board_lcd() 返回对象。
 *
 * 注意：这里不绘制启动画面；app_boot_display() 通过 board_lcd()
 * 拿到设备后再绘图。LCD 未启用时直接返回 FW_OK。
 */
static fw_status_t board_display_init(void)
{
#if defined(BOARD_LCD_ENABLE) && BOARD_LCD_ENABLE
    fw_dev_t *pin = fw_dev_find("pin");
    fw_dev_t *spi = lcd_spi_dev();
    st7735s_config_t config;
    fw_status_t result;

    if (pin == NULL || spi == NULL) return FW_ENOTREADY;
    result = lcd_pins_init(pin);
    if (result != FW_OK) return result;

    config.spi = spi;
    config.pin = pin;
    config.cs = lcd_pin(s_lcd_cfg.cs);
    config.dc = lcd_pin(s_lcd_cfg.dc);
    config.res = lcd_pin(s_lcd_cfg.res);
    config.bl = lcd_pin(s_lcd_cfg.bl);
    config.bl_active_high = s_lcd_cfg.bl_active_high;
    config.width = s_lcd_cfg.width;
    config.height = s_lcd_cfg.height;
    config.x_offset = s_lcd_cfg.x_offset;
    config.y_offset = s_lcd_cfg.y_offset;
    config.madctl = s_lcd_cfg.madctl;
    config.invert = s_lcd_cfg.invert;

    result = st7735s_init(&s_lcd, "lcd", &config);
    if (result != FW_OK) return result;
    s_lcd_ready = true;
#endif
    return FW_OK;
}

/* ====================================================================== */
/* 7. 公共启动入口                                                       */
/* ====================================================================== */

/*
 * RT-Thread 在调度器启动前调用这个函数（由启动文件/rtthread_startup
 * 进入）。这里完成两件事：
 *
 *   1. stm32f407_boot_init(&s_clock)：
 *        HAL_Init、NVIC 分组、时钟树、SysTick、堆/延时钩子、复位原因；
 *   2. board_devices_init()：
 *        fault/DMA、接线校验、pin、UART/SPI/I2C/ADC/PWM、NVM、LCD。
 *
 * 任一步失败直接 panic，不进入 RT-Thread 组件初始化。此时控制台可能
 * 尚未绑定，panic 会尽量直写串口；如果串口也失败，只能停机。
 *
 * 最后调用 rt_components_board_init() 执行 INIT_BOARD_EXPORT 等
 * 自动初始化段（如果内核启用 RT_USING_COMPONENTS_INIT）。
 */
void rt_hw_board_init(void)
{
    if (stm32f407_boot_init(&s_clock) != FW_OK)
        stm32f407_panic(__FILE__, __LINE__);
    if (board_devices_init() != FW_OK)
        stm32f407_panic(__FILE__, __LINE__);

#ifdef RT_USING_COMPONENTS_INIT
    rt_components_board_init();
#endif
}

/*
 * 板级设备初始化总入口（幂等）。
 *
 * 顺序不是随意的，而是依赖顺序：
 *
 *   system   fault 钩子 + DMA owner 表（后续任何 panic 都能输出）
 *   pins     结构校验（在真正改 GPIO 前拒绝明显错误）
 *   pin      "pin" 设备（LED/LCD 依赖）
 *   led      LED 输出（失败也不阻塞后续外设，但当前按致命处理）
 *   serial   serialN + 控制台绑定（后续日志/panic 可用）
 *   spi      spiN（LCD 依赖）
 *   i2c      i2cN
 *   adc      adc1
 *   pwm      pwm4
 *   storage  NVM 后端 + fw_param 扫描
 *   display  "lcd"（依赖 pin + spi）
 *
 * BOARD_RUN 宏负责统一错误路径：打印失败步骤名和状态码，然后返回。
 * 不能在宏里 panic，因为 board_devices_init 也可能在测试或非启动
 * 上下文调用；是否 panic 由 rt_hw_board_init 决定。
 *
 * 重复调用安全：第一次成功后 s_devices_ready=true，后续直接返回。
 */
fw_status_t board_devices_init(void)
{
    fw_status_t result;

    if (s_devices_ready) return FW_OK;

#define BOARD_RUN(name, step)                                             \
    do {                                                                  \
        result = (step);                                                  \
        if (result != FW_OK) {                                            \
            rt_kprintf("board init failed: %s (%s)\n", name,              \
                       fw_status_name(result));                           \
            return result;                                                \
        }                                                                 \
    } while (0)

    /* 顺序即依赖：system → 接线校验 → pin → LED → serial → 总线外设 → LCD。 */
    BOARD_RUN("system", board_system_init());
    BOARD_RUN("pins", board_pins_validate());
    BOARD_RUN("pin", board_pin_init());
    BOARD_RUN("led", board_led_init());
    BOARD_RUN("serial", board_serial_init());
    BOARD_RUN("spi", board_spi_init());
    BOARD_RUN("i2c", board_i2c_init());
    BOARD_RUN("adc", board_adc_init());
    BOARD_RUN("pwm", board_pwm_init());
    BOARD_RUN("storage", board_storage_init());
    BOARD_RUN("display", board_display_init());

#undef BOARD_RUN

    s_devices_ready = true;
    return FW_OK;
}

/* ====================================================================== */
/* 8. 应用可见的板级角色                                                 */
/* ====================================================================== */

/*
 * 打开所有已注册 UART 的 RX/NVIC 中断。
 *
 * 为什么不在 board_serial_init 里直接打开：
 *   启动早期控制台可能还没完成注册，应用也还没准备好接收数据。让
 *   main 在所有设备/任务就绪后显式调用，时序更可控，也方便测试。
 *
 * 未完成 board_devices_init 时返回 FW_ENOTREADY。
 */
fw_status_t board_serial_start_all(void)
{
    if (!s_devices_ready) return FW_ENOTREADY;
    return drv_serial_start_all();
}

/*
 * 返回控制台 serial 设备。
 *
 * 应用通常在 main 开始时调用 fw_dev_open() 打开这个设备；返回值是
 * 框架设备指针，不是 drv_serial_t。未初始化或实例不存在时返回 NULL。
 */
fw_dev_t *board_console_dev(void)
{
    return s_devices_ready ? drv_serial_dev(BOARD_CONSOLE_UART) : NULL;
}

/*
 * 返回唯一的 "pin" 设备。
 *
 * 应用用它与 fw_pin_configure/fw_pin_write 交互；board 内部也用它
 * 配置 LED/LCD 控制脚。
 */
fw_dev_t *board_pin_dev(void)
{
    return s_devices_ready ? fw_dev_find("pin") : NULL;
}

/*
 * LED 能力查询。
 *
 * board_led_enabled()  — 当前 profile 是否启用 LED（决定应用是否绑定
 *                         LED 命令）；
 * board_led_pin()      — FW_PIN_NUMBER 编码的 LED 引脚，未启用返回 -1；
 * board_led_active_high() — 高电平是否点亮。
 */
bool board_led_enabled(void)
{
#if defined(BOARD_LED_ENABLE) && BOARD_LED_ENABLE
    return true;
#else
    return false;
#endif
}

/*
 * 返回 LED 引脚编号（FW_PIN_NUMBER 编码）。
 *
 * 应用如果不想通过 board_pin_dev() 自己查表，可以直接用这个值调用
 * fw_pin_write/fw_pin_configure。LED 未启用时返回 -1，应用应先用
 * board_led_enabled() 判断。
 */
int board_led_pin(void)
{
#if defined(BOARD_LED_ENABLE) && BOARD_LED_ENABLE
    return FW_PIN_NUMBER(s_pins[BOARD_PIN_LED].port,
                         s_pins[BOARD_PIN_LED].pin);
#else
    return -1;
#endif
}

/*
 * LED 是否高电平点亮。
 *
 * 返回 false 时应用应把“亮”翻译成 GPIO 输出 0。
 * LED 未启用时返回 false，保持语义稳定。
 */
bool board_led_active_high(void)
{
#if defined(BOARD_LED_ENABLE) && BOARD_LED_ENABLE
    return BOARD_LED_ACTIVE_HIGH != 0;
#else
    return false;
#endif
}

/*
 * 返回 LCD 显示对象。
 *
 * 只有 board_display_init() 成功完成 st7735s_init() 后才返回非 NULL；
 * LCD 未启用、SPI 实例未注册或面板初始化失败时返回 NULL。
 *
 * 应用（app_boot_display）在绘图前应该判断返回值；返回 NULL 时跳过
 * 显示步骤，不影响串口/任务等其他功能。
 */
fw_display_t *board_lcd(void)
{
#if defined(BOARD_LCD_ENABLE) && BOARD_LCD_ENABLE
    return s_lcd_ready ? &s_lcd.base : NULL;
#else
    return NULL;
#endif
}

/* ====================================================================== */
/* 9. 板卡信息、复位原因与看门狗                                          */
/* ====================================================================== */

/*
 * 板卡身份。字符串来自 board_config.h 的 BOARD_NAME/BOARD_VERSION，
 * 应用用于启动横幅/诊断输出。
 */
const char *board_name(void)
{
    return BOARD_NAME;
}

const char *board_version(void)
{
    return BOARD_VERSION;
}

/*
 * 上一次复位原因（RCC_CSR 位图）。
 *
 * 启动早期由 platform/stm32f407 捕获并清零标志；这里只是转出。
 * 应用可用 board_reset_cause_name() 转成可读字符串，例如判断是
 * 上电、看门狗还是软件复位。
 */
uint32_t board_reset_cause(void)
{
    return stm32f407_reset_cause();
}

/*
 * 复位原因位图 -> 可读名称（未知返回 "UNKNOWN"）。
 */
const char *board_reset_cause_name(uint32_t cause)
{
    return stm32f407_reset_cause_name(cause);
}

/*
 * 看门狗能力查询。
 *
 * BOARD_IWDG_ENABLE=0 时：
 *   - board_watchdog_enabled() 返回 false；
 *   - start 返回 FW_OK（no-op），feed 也是 no-op。
 * 这样应用不需要写 #if，统一按“看门狗总是存在”的接口调用。
 */
bool board_watchdog_enabled(void)
{
#if defined(BOARD_IWDG_ENABLE) && BOARD_IWDG_ENABLE
    return true;
#else
    return false;
#endif
}

/*
 * 启动 IWDG。
 *
 * 必须在所有设备/任务就绪后调用；IWDG 一旦启动不可停止。
 * 超时时间来自 BOARD_IWDG_TIMEOUT_MS（LSI 32kHz 估算，实际值
 * 随芯片偏差浮动）。未启用时返回 FW_OK，调用者无需分支。
 */
fw_status_t board_watchdog_start(void)
{
#if defined(BOARD_IWDG_ENABLE) && BOARD_IWDG_ENABLE
    return stm32f407_iwdg_start(BOARD_IWDG_TIMEOUT_MS);
#else
    return FW_OK;
#endif
}

/*
 * 喂狗。
 *
 * 由 main 的监督循环在 fw_task_summary().lost == 0 时调用；如果
 * 有任务失联或 main 自身卡死，停止喂狗，让 IWDG 复位系统。
 * 未启用时是 no-op。
 */
void board_watchdog_feed(void)
{
#if defined(BOARD_IWDG_ENABLE) && BOARD_IWDG_ENABLE
    stm32f407_iwdg_feed();
#endif
}
