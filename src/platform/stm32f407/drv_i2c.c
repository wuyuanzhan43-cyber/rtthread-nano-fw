/*
 * drv_i2c.c - STM32 I2C 实例表驱动。
 *
 * 层级：src/platform/stm32f407
 * 职责：
 *   1. 从板级描述符配置 SCL/SDA 复用开漏引脚；
 *   2. 初始化 HAL I2C 主模式；
 *   3. 实现 configure/transfer/mem_read/mem_write/probe；
 *   4. 注册 i2c1..i2c3。
 *
 * 上下文：任务上下文，全部阻塞；不使用中断/DMA。
 * 依赖：drv_i2c.h、fw_config.h、stm32f407_map.h、drv_common.h。
 *
 * 地址约定：
 *   类层传 7 位地址；HAL 的 DevAddress 是 8 位格式（7 位 << 1），
 *   因此驱动统一左移一位后再调用 HAL。
 */
#include "drv_i2c.h"
#include "fw_config.h"
#include "stm32f407_map.h"
#include "drv_common.h"

/* 配置一个 I2C 复用引脚：开漏 + 上拉。 */
static fw_status_t configure_pin(uint8_t port_index, uint8_t pin_number,
                                 uint8_t af)
{
    GPIO_TypeDef *port = stm32f407_gpio(port_index);
    GPIO_InitTypeDef init = {0};
    if (port == NULL || pin_number > 15U) return FW_EINVAL;
    if (stm32f407_gpio_clock_enable(port_index) != FW_OK) return FW_EINVAL;
    init.Pin = (uint16_t)(1U << pin_number);
    init.Mode = GPIO_MODE_AF_OD;
    init.Pull = GPIO_PULLUP;
    init.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    init.Alternate = af;
    HAL_GPIO_Init(port, &init);
    return FW_OK;
}

/*
 * 应用总线频率。
 *
 * 只修改 ClockSpeed 并重新 HAL_I2C_Init；引脚复用保持不变。
 * HAL 会根据 PCLK1 自动计算 CCR/TRISE。
 */
static int i2c_configure(fw_dev_t *dev, const fw_i2c_config_t *config)
{
    drv_i2c_t *i2c = FW_CONTAINER_OF(dev, drv_i2c_t, base.dev);
    if (i2c == NULL || config == NULL || config->clock_hz == 0U)
        return FW_EINVAL;
    /* HAL 的状态机在传输中会拒绝 Init；显式返回 EBUSY 更清晰。 */
    if (i2c->hal.State != HAL_I2C_STATE_READY) return FW_EBUSY;

    i2c->hal.Init.ClockSpeed = config->clock_hz;
    if (HAL_I2C_Init(&i2c->hal) != HAL_OK) return FW_EIO;
    return FW_OK;
}

/* 7 位地址 -> HAL 的 8 位地址格式。 */
static uint16_t hal_address(uint16_t addr7)
{
    return (uint16_t)(addr7 << 1);
}

/*
 * 原始事务。
 *
 * tx/rx 都非空时先写后读，两个事务之间会有 STOP；
 * 需要 repeated start 的寄存器读请使用 mem_read。
 */
static int i2c_transfer(fw_dev_t *dev, uint16_t addr7,
                        const uint8_t *tx, size_t tx_len,
                        uint8_t *rx, size_t rx_len, uint32_t timeout_ms)
{
    drv_i2c_t *i2c = FW_CONTAINER_OF(dev, drv_i2c_t, base.dev);
    HAL_StatusTypeDef status;
    uint16_t address;

    if (i2c == NULL) return FW_EINVAL;
    if (tx_len == 0U && rx_len == 0U) return FW_EINVAL;
    if (tx_len > FW_I2C_MAX_TRANSFER || rx_len > FW_I2C_MAX_TRANSFER)
        return FW_EINVAL;
    if (tx_len != 0U && tx == NULL) return FW_EINVAL;
    if (rx_len != 0U && rx == NULL) return FW_EINVAL;
    address = hal_address(addr7);

    if (tx_len != 0U) {
        status = HAL_I2C_Master_Transmit(&i2c->hal, address, (uint8_t *)tx,
                                         (uint16_t)tx_len,
                                         drv_timeout_to_hal(timeout_ms));
        if (status != HAL_OK) return (int)drv_status_from_hal(status);
    }
    if (rx_len != 0U) {
        status = HAL_I2C_Master_Receive(&i2c->hal, address, rx,
                                        (uint16_t)rx_len,
                                        drv_timeout_to_hal(timeout_ms));
        if (status != HAL_OK) return (int)drv_status_from_hal(status);
    }
    return (int)(tx_len + rx_len);
}

/* 寄存器读：HAL 内部为 repeated start。 */
static int i2c_mem_read(fw_dev_t *dev, uint16_t addr7, uint16_t mem_addr,
                        uint8_t mem_addr_size, uint8_t *rx, size_t rx_len,
                        uint32_t timeout_ms)
{
    drv_i2c_t *i2c = FW_CONTAINER_OF(dev, drv_i2c_t, base.dev);
    HAL_StatusTypeDef status;
    if (i2c == NULL || rx == NULL || rx_len == 0U ||
        rx_len > FW_I2C_MAX_TRANSFER)
        return FW_EINVAL;
    if (mem_addr_size != 1U && mem_addr_size != 2U) return FW_EINVAL;

    status = HAL_I2C_Mem_Read(&i2c->hal, hal_address(addr7), mem_addr,
                              mem_addr_size == 1U ? I2C_MEMADD_SIZE_8BIT
                                                  : I2C_MEMADD_SIZE_16BIT,
                              rx, (uint16_t)rx_len,
                              drv_timeout_to_hal(timeout_ms));
    return status == HAL_OK ? (int)rx_len : (int)drv_status_from_hal(status);
}

/* 寄存器写。 */
static int i2c_mem_write(fw_dev_t *dev, uint16_t addr7, uint16_t mem_addr,
                         uint8_t mem_addr_size, const uint8_t *tx,
                         size_t tx_len, uint32_t timeout_ms)
{
    drv_i2c_t *i2c = FW_CONTAINER_OF(dev, drv_i2c_t, base.dev);
    HAL_StatusTypeDef status;
    if (i2c == NULL || tx == NULL || tx_len == 0U ||
        tx_len > FW_I2C_MAX_TRANSFER)
        return FW_EINVAL;
    if (mem_addr_size != 1U && mem_addr_size != 2U) return FW_EINVAL;

    status = HAL_I2C_Mem_Write(&i2c->hal, hal_address(addr7), mem_addr,
                               mem_addr_size == 1U ? I2C_MEMADD_SIZE_8BIT
                                                   : I2C_MEMADD_SIZE_16BIT,
                               (uint8_t *)tx, (uint16_t)tx_len,
                               drv_timeout_to_hal(timeout_ms));
    return status == HAL_OK ? (int)tx_len : (int)drv_status_from_hal(status);
}

/*
 * 设备探测。
 *
 * HAL 对无应答通常返回 HAL_TIMEOUT，这里映射为 FW_ENODEV；
 * 总线被拉低卡死时也可能超时，调用者需要结合硬件判断。
 */
static int i2c_probe(fw_dev_t *dev, uint16_t addr7, uint32_t timeout_ms)
{
    drv_i2c_t *i2c = FW_CONTAINER_OF(dev, drv_i2c_t, base.dev);
    HAL_StatusTypeDef status;
    if (i2c == NULL) return FW_EINVAL;

    status = HAL_I2C_IsDeviceReady(&i2c->hal, hal_address(addr7), 3U,
                                   drv_timeout_to_hal(timeout_ms));
    if (status == HAL_OK) return FW_OK;
    if (status == HAL_TIMEOUT) return FW_ENODEV;
    return (int)drv_status_from_hal(status);
}

static const fw_i2c_ops_t drv_i2c_ops = {
    i2c_configure,
    i2c_transfer,
    i2c_mem_read,
    i2c_mem_write,
    i2c_probe
};

/* 初始化一个 I2C 实例。 */
static fw_status_t i2c_init_instance(drv_i2c_t *i2c, const char *name,
                                     const drv_i2c_hw_t *hw)
{
    fw_i2c_config_t config;
    fw_status_t result;
    uint8_t instance;

    if (i2c == NULL || name == NULL || hw == NULL) return FW_EINVAL;
    instance = hw->instance;
    if (instance < 1U || instance > 3U || hw->clock_hz == 0U)
        return FW_EINVAL;

    result = configure_pin(hw->scl_port, hw->scl_pin, hw->scl_af);
    if (result != FW_OK) return result;
    result = configure_pin(hw->sda_port, hw->sda_pin, hw->sda_af);
    if (result != FW_OK) return result;

    stm32f407_i2c_clock_enable(instance);
    i2c->instance = instance;
    i2c->hal.Instance = stm32f407_i2c(instance);
    if (i2c->hal.Instance == NULL) return FW_EINVAL;
    i2c->hal.Init.ClockSpeed = hw->clock_hz;
    i2c->hal.Init.DutyCycle = I2C_DUTYCYCLE_2;
    i2c->hal.Init.OwnAddress1 = 0U;
    i2c->hal.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
    i2c->hal.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
    i2c->hal.Init.OwnAddress2 = 0U;
    i2c->hal.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
    i2c->hal.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
    if (HAL_I2C_Init(&i2c->hal) != HAL_OK) return FW_EIO;

    config.clock_hz = hw->clock_hz;
    i2c->drv.type = FW_DEV_TYPE_I2C;
    i2c->drv.ops = &drv_i2c_ops;
    i2c->drv.data = i2c;
    return fw_i2c_register(&i2c->base, name, &i2c->drv, &config);
}

#if defined(FW_USING_I2C1) && FW_USING_I2C1
static drv_i2c_t i2c1;
#endif
#if defined(FW_USING_I2C2) && FW_USING_I2C2
static drv_i2c_t i2c2;
#endif
#if defined(FW_USING_I2C3) && FW_USING_I2C3
static drv_i2c_t i2c3;
#endif

static drv_i2c_t *i2c_slot(uint8_t instance)
{
    switch (instance) {
#if defined(FW_USING_I2C1) && FW_USING_I2C1
    case 1: return &i2c1;
#endif
#if defined(FW_USING_I2C2) && FW_USING_I2C2
    case 2: return &i2c2;
#endif
#if defined(FW_USING_I2C3) && FW_USING_I2C3
    case 3: return &i2c3;
#endif
    default: return NULL;
    }
}

static const char *i2c_name(uint8_t instance)
{
    switch (instance) {
    case 1: return "i2c1";
    case 2: return "i2c2";
    case 3: return "i2c3";
    default: return NULL;
    }
}

fw_status_t drv_i2c_register_all(const drv_i2c_hw_t *hw, size_t count)
{
    fw_status_t result;
    size_t i;

    if (count == 0U) return FW_OK;
    if (hw == NULL) return FW_EINVAL;

    for (i = 0U; i < count; ++i) {
        drv_i2c_t *slot = i2c_slot(hw[i].instance);
        const char *name = i2c_name(hw[i].instance);
        if (slot == NULL || name == NULL) return FW_ENOTSUP;
        result = i2c_init_instance(slot, name, &hw[i]);
        if (result != FW_OK) return result;
    }
    return FW_OK;
}
