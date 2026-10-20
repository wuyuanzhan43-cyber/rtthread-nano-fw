/*
 * drv_spi.c - STM32 SPI 实例表驱动。
 *
 * 层级：src/bsp/st/driver
 * 职责：
 *   1. 按 Kconfig 逐实例配置 SCK/MOSI/MISO 复用引脚；
 *   2. 计算 PCLK 分频并初始化 HAL SPI；
 *   3. 实现 configure/transfer 两个类 ops；
 *   4. 注册 spi1..N 设备。
 *
 * 上下文：任务上下文，允许阻塞在 HAL 轮询传输上。
 * 依赖：drv_spi.h、board_config.h、fw_config.h、stm32f407_map.h。
 *
 * SPI 参数：
 *   - 主模式、8 位、软件 NSS；
 *   - 模式 0（CPOL=0, CPHA=0）作为默认；
 *   - MSB first；
 *   - 频率由 stm32f407_spi_prescaler 选择不超过 max_hz 的最小分频。
 *
 * 与 serial 不同，SPI 没有 ring 和中断：
 *   transfer 是同步调用，HAL 内部轮询 TXE/RXNE；
 *   LCD 一帧数据量最大 128*160*2=40KB，但类层按 32/64 字节分块，
 *   单次 HAL 调用不会过长。
 */
#include "drv_spi.h"
#include "board_config.h"
#include "fw_config.h"
#include "stm32f407_map.h"

/*
 * 毫秒超时到 HAL 超时的转换。
 *   UINT32_MAX -> HAL_MAX_DELAY（永久等待）；
 *   0          -> 1（HAL 不接受 0，给最小非零值）；
 *   其他       -> 原值（HAL 以毫秒为单位）。
 */
static uint32_t timeout_to_hal(uint32_t timeout_ms)
{
    if (timeout_ms == UINT32_MAX) return HAL_MAX_DELAY;
    if (timeout_ms == 0U) return 1U;
    return timeout_ms;
}

/*
 * 配置一个 SPI 复用引脚。
 *
 * SCK/MOSI/MISO 都使用推挽复用、最高速度、无上下拉。
 * 片选不在这里配置：它由具体设备作为普通 GPIO 控制，
 * 这样同一总线上的多个从设备可以共用 SPI。
 */
static fw_status_t configure_pin(uint8_t port_index, uint8_t pin_number,
                                 uint8_t af)
{
    GPIO_TypeDef *port = stm32f407_gpio(port_index);
    GPIO_InitTypeDef init = {0};
    if (port == NULL || pin_number > 15U) return FW_EINVAL;
    if (stm32f407_gpio_clock_enable(port_index) != FW_OK) return FW_EINVAL;
    init.Pin = (uint16_t)(1U << pin_number);
    init.Mode = GPIO_MODE_AF_PP;
    init.Pull = GPIO_NOPULL;
    init.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    init.Alternate = af;
    HAL_GPIO_Init(port, &init);
    return FW_OK;
}

/*
 * 应用 SPI 配置。
 *
 * 参数：
 *   max_hz    - 频率上限，驱动向下选择分频；
 *   mode      - 0..3，bit1=CPOL，bit0=CPHA：
 *                 0: 空闲低、第一边沿采样
 *                 1: 空闲低、第二边沿采样
 *                 2: 空闲高、第一边沿采样
 *                 3: 空闲高、第二边沿采样
 *   bit_order - 0=LSB first，1=MSB first。
 *
 * 修改配置会重新调用 HAL_SPI_Init，并重新使能外设。
 * 调用者应确保当前没有正在进行的传输（SPI 类不提供并发保护）。
 */
static int spi_configure(fw_dev_t *dev, const fw_spi_config_t *config)
{
    drv_spi_t *spi = FW_CONTAINER_OF(dev, drv_spi_t, base.dev);
    HAL_StatusTypeDef status;
    if (spi == NULL || config == NULL || config->max_hz == 0U)
        return FW_EINVAL;

    spi->hal.Init.BaudRatePrescaler =
        stm32f407_spi_prescaler(spi->pclk, config->max_hz);
    spi->hal.Init.CLKPolarity = (config->mode & 2) != 0
                                    ? SPI_POLARITY_HIGH : SPI_POLARITY_LOW;
    spi->hal.Init.CLKPhase = (config->mode & 1) != 0
                                 ? SPI_PHASE_2EDGE : SPI_PHASE_1EDGE;
    spi->hal.Init.FirstBit = config->bit_order != 0
                                 ? SPI_FIRSTBIT_MSB : SPI_FIRSTBIT_LSB;
    status = HAL_SPI_Init(&spi->hal);
    if (status != HAL_OK) return FW_EIO;
    __HAL_SPI_ENABLE(&spi->hal);
    return FW_OK;
}

/*
 * 同步传输。
 *
 * 三种模式：
 *   tx && rx   -> HAL_SPI_TransmitReceive（全双工，常用于读寄存器）；
 *   tx only    -> HAL_SPI_Transmit（LCD 写命令/像素）；
 *   rx only    -> HAL_SPI_Receive（读传感器/Flash）。
 *
 * 返回 length 或 FW_EIO；调用者通过返回值判断是否完整传输。
 * 注意：HAL 的 Size 参数是 uint16_t，length 超过 65535 会被截断；
 * 当前类层的分块远小于该值，但驱动仍做一次显式转换。
 */
static int spi_transfer(fw_dev_t *dev, const uint8_t *tx, uint8_t *rx,
                        size_t length, uint32_t timeout_ms)
{
    drv_spi_t *spi = FW_CONTAINER_OF(dev, drv_spi_t, base.dev);
    HAL_StatusTypeDef status;
    if (spi == NULL || length == 0U) return FW_EINVAL;

    if (tx != NULL && rx != NULL) {
        status = HAL_SPI_TransmitReceive(&spi->hal, (uint8_t *)tx, rx,
                                         (uint16_t)length,
                                         timeout_to_hal(timeout_ms));
    } else if (tx != NULL) {
        status = HAL_SPI_Transmit(&spi->hal, (uint8_t *)tx, (uint16_t)length,
                                  timeout_to_hal(timeout_ms));
    } else if (rx != NULL) {
        status = HAL_SPI_Receive(&spi->hal, rx, (uint16_t)length,
                                 timeout_to_hal(timeout_ms));
    } else {
        return FW_EINVAL;
    }
    return status == HAL_OK ? (int)length : FW_EIO;
}

/* SPI 驱动 ops。 */
static const fw_spi_ops_t drv_spi_ops = {
    spi_configure,
    spi_transfer
};

/*
 * 初始化一个 SPI 实例。
 *
 * 步骤：
 *   1. 校验实例编号 1..3；
 *   2. 配置 SCK/MOSI，必要时配置 MISO；
 *   3. 使能 SPI 时钟，计算该实例的 PCLK；
 *   4. 填写 HAL 初始化参数（主模式、8 位、软件 NSS）；
 *   5. HAL_SPI_Init + 使能；
 *   6. 填写驱动描述并注册 spi 类设备。
 *
 * 初始配置保存为 max_hz、mode=0、bit_order=1（MSB）；
 * 设备可以在运行期通过 ioctl 修改。
 */
static fw_status_t spi_init_instance(drv_spi_t *spi, const char *name,
                                     uint8_t instance,
                                     uint8_t sck_port, uint8_t sck_pin,
                                     uint8_t mosi_port, uint8_t mosi_pin,
                                     bool use_miso, uint8_t miso_port,
                                     uint8_t miso_pin, uint8_t af,
                                     uint32_t max_hz)
{
    fw_spi_config_t config;
    fw_status_t result;
    SPI_InitTypeDef *init;
    if (spi == NULL || name == NULL || instance < 1U || instance > 3U)
        return FW_EINVAL;

    result = configure_pin(sck_port, sck_pin, af);
    if (result != FW_OK) return result;
    result = configure_pin(mosi_port, mosi_pin, af);
    if (result != FW_OK) return result;
    if (use_miso) {
        result = configure_pin(miso_port, miso_pin, af);
        if (result != FW_OK) return result;
    }

    stm32f407_spi_clock_enable(instance);
    spi->instance = instance;
    spi->pclk = stm32f407_spi_pclk(instance);
    spi->hal.Instance = stm32f407_spi(instance);
    init = &spi->hal.Init;
    init->Mode = SPI_MODE_MASTER;
    init->Direction = use_miso ? SPI_DIRECTION_2LINES : SPI_DIRECTION_2LINES;
    init->DataSize = SPI_DATASIZE_8BIT;
    init->CLKPolarity = SPI_POLARITY_LOW;
    init->CLKPhase = SPI_PHASE_1EDGE;
    init->NSS = SPI_NSS_SOFT;
    init->BaudRatePrescaler = stm32f407_spi_prescaler(spi->pclk, max_hz);
    init->FirstBit = SPI_FIRSTBIT_MSB;
    init->TIMode = SPI_TIMODE_DISABLE;
    init->CRCCalculation = SPI_CRCCALCULATION_DISABLE;
    init->CRCPolynomial = 10U;
    if (HAL_SPI_Init(&spi->hal) != HAL_OK) return FW_EIO;
    __HAL_SPI_ENABLE(&spi->hal);

    config.max_hz = max_hz;
    config.mode = 0;
    config.bit_order = 1; /* MSB first，LCD 常用 */

    spi->drv.type = FW_DEV_TYPE_SPI;
    spi->drv.ops = &drv_spi_ops;
    spi->drv.data = spi;
    return fw_spi_register(&spi->base, name, &spi->drv, &config);
}

/*
 * 静态实例定义。
 * 只有 Kconfig 启用对应 SPI 时才分配结构体；关闭的实例不占 RAM。
 */
#if defined(FW_USING_SPI1) && FW_USING_SPI1
static drv_spi_t spi1;
#endif
#if defined(FW_USING_SPI2) && FW_USING_SPI2
static drv_spi_t spi2;
#endif
#if defined(FW_USING_SPI3) && FW_USING_SPI3
static drv_spi_t spi3;
#endif

/*
 * 注册所有已启用的 SPI。
 *
 * 引脚、AF 和最大频率都来自 board_config.h（Kconfig 生成），
 * 因此新增 SPI 实例同样只需要改 Kconfig。
 */
fw_status_t drv_spi_register_all(void)
{
    fw_status_t result = FW_OK;
#if defined(FW_USING_SPI1) && FW_USING_SPI1
    result = spi_init_instance(&spi1, "spi1", 1,
                               BOARD_SPI1_SCK_PORT, BOARD_SPI1_SCK_PIN,
                               BOARD_SPI1_MOSI_PORT, BOARD_SPI1_MOSI_PIN,
                               BOARD_SPI1_USE_MISO != 0,
                               BOARD_SPI1_MISO_PORT, BOARD_SPI1_MISO_PIN,
                               BOARD_SPI1_AF, BOARD_SPI1_MAX_HZ);
    if (result != FW_OK) return result;
#endif
#if defined(FW_USING_SPI2) && FW_USING_SPI2
    result = spi_init_instance(&spi2, "spi2", 2,
                               BOARD_SPI2_SCK_PORT, BOARD_SPI2_SCK_PIN,
                               BOARD_SPI2_MOSI_PORT, BOARD_SPI2_MOSI_PIN,
                               BOARD_SPI2_USE_MISO != 0,
                               BOARD_SPI2_MISO_PORT, BOARD_SPI2_MISO_PIN,
                               BOARD_SPI2_AF, BOARD_SPI2_MAX_HZ);
    if (result != FW_OK) return result;
#endif
#if defined(FW_USING_SPI3) && FW_USING_SPI3
    result = spi_init_instance(&spi3, "spi3", 3,
                               BOARD_SPI3_SCK_PORT, BOARD_SPI3_SCK_PIN,
                               BOARD_SPI3_MOSI_PORT, BOARD_SPI3_MOSI_PIN,
                               BOARD_SPI3_USE_MISO != 0,
                               BOARD_SPI3_MISO_PORT, BOARD_SPI3_MISO_PIN,
                               BOARD_SPI3_AF, BOARD_SPI3_MAX_HZ);
    if (result != FW_OK) return result;
#endif
    return result;
}
