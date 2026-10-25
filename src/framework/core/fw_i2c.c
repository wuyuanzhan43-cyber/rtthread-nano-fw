/*
 * fw_i2c.c - I2C 设备类的通用实现。
 *
 * 层级：src/framework/core
 * 职责：
 *   1. 校验 7 位地址、长度和 mem_addr_size；
 *   2. 把 read/write/ioctl 和 helper 转发到驱动 ops；
 *   3. 保存总线配置的软件副本。
 *
 * 上下文：任务上下文，全部阻塞。
 * 依赖：fw/i2c.h。
 *
 * 设计说明：
 *   类层不包含 HAL/RTOS；地址在类层保持 7 位，驱动负责左移成 HAL 的
 *   8 位地址格式。transfer 的写后读是两个独立事务；需要 repeated start
 *   的寄存器读走 mem_read。
 */
#include "fw/i2c.h"

/* 从基类取驱动 ops，任何一层为空都返回 NULL。 */
static const fw_i2c_ops_t *i2c_ops(const fw_dev_t *dev)
{
    if (dev == NULL || dev->drv == NULL || dev->drv->ops == NULL) return NULL;
    return (const fw_i2c_ops_t *)dev->drv->ops;
}

/* F407 的 I2C 只支持 7 位地址。 */
static bool addr_ok(uint16_t addr7)
{
    return addr7 <= 0x7FU;
}

static bool mem_size_ok(uint8_t mem_addr_size)
{
    return mem_addr_size == 1U || mem_addr_size == 2U;
}

/*
 * 设备 read/write 入口。
 *
 * off 是 7 位从机地址；read 只收，write 只发。
 * fw_dev 核心已经处理 buf/size 为空的情况。
 */
static int i2c_read(fw_dev_t *dev, int off, void *buf, size_t size,
                    uint32_t timeout_ms)
{
    if (off < 0 || off > 0x7F) return FW_EINVAL;
    return fw_i2c_transfer(dev, (uint16_t)off, NULL, 0U,
                           (uint8_t *)buf, size, timeout_ms);
}

static int i2c_write(fw_dev_t *dev, int off, const void *buf, size_t size,
                     uint32_t timeout_ms)
{
    if (off < 0 || off > 0x7F) return FW_EINVAL;
    return fw_i2c_transfer(dev, (uint16_t)off, (const uint8_t *)buf, size,
                           NULL, 0U, timeout_ms);
}

/*
 * SET_CONFIG:
 *   - clock_hz 必须在 10 kHz..400 kHz；
 *   - 先调用驱动 configure，成功后才更新软件副本。
 * GET_CONFIG: 返回软件副本。
 */
static int i2c_ioctl(fw_dev_t *dev, int off, int cmd, void *args)
{
    fw_i2c_t *i2c = FW_CONTAINER_OF(dev, fw_i2c_t, dev);
    const fw_i2c_ops_t *ops = i2c_ops(dev);
    (void)off;

    if (cmd == FW_CTL_I2C_SET_CONFIG) {
        fw_i2c_config_t config;
        if (args == NULL || ops == NULL || ops->configure == NULL)
            return FW_EINVAL;
        config = *(const fw_i2c_config_t *)args;
        if (config.clock_hz < 10000U || config.clock_hz > 400000U)
            return FW_EINVAL;
        {
            int result = ops->configure(dev, &config);
            if (result < 0) return result;
        }
        i2c->config = config;
        return (int)sizeof(config);
    }

    if (cmd == FW_CTL_I2C_GET_CONFIG) {
        if (args == NULL) return FW_EINVAL;
        *(fw_i2c_config_t *)args = i2c->config;
        return (int)sizeof(i2c->config);
    }
    return FW_ENOTSUP;
}

static const fw_dev_ops_t i2c_dev_ops = {
    NULL,
    NULL,
    i2c_read,
    i2c_write,
    i2c_ioctl,
    NULL
};

/* 注册：configure/transfer 必需，mem_* 与 probe 可选。 */
fw_status_t fw_i2c_register(fw_i2c_t *i2c, const char *name,
                            const fw_drv_t *drv,
                            const fw_i2c_config_t *config)
{
    const fw_i2c_ops_t *ops;
    fw_status_t result;

    if (i2c == NULL || name == NULL || drv == NULL || drv->ops == NULL ||
        config == NULL) return FW_EINVAL;
    if (drv->type != FW_DEV_TYPE_I2C) return FW_EINVAL;

    ops = (const fw_i2c_ops_t *)drv->ops;
    if (ops->configure == NULL || ops->transfer == NULL) return FW_EINVAL;
    if (config->clock_hz < 10000U || config->clock_hz > 400000U)
        return FW_EINVAL;

    result = fw_dev_register(&i2c->dev, name, FW_DEV_TYPE_I2C, drv);
    if (result != FW_OK) return result;

    i2c->dev.ops = &i2c_dev_ops;
    i2c->config = *config;
    return FW_OK;
}

fw_i2c_t *fw_i2c_find(const char *name)
{
    fw_dev_t *dev = fw_dev_find(name);
    if (dev == NULL || dev->type != FW_DEV_TYPE_I2C) return NULL;
    return FW_CONTAINER_OF(dev, fw_i2c_t, dev);
}

/* 原始传输；返回总字节数。 */
int fw_i2c_transfer(fw_dev_t *dev, uint16_t addr7,
                    const uint8_t *tx, size_t tx_len,
                    uint8_t *rx, size_t rx_len, uint32_t timeout_ms)
{
    const fw_i2c_ops_t *ops = i2c_ops(dev);
    if (ops == NULL || ops->transfer == NULL) return FW_ENOTSUP;
    if (!addr_ok(addr7)) return FW_EINVAL;
    if (tx_len == 0U && rx_len == 0U) return FW_EINVAL;
    if (tx_len > FW_I2C_MAX_TRANSFER || rx_len > FW_I2C_MAX_TRANSFER)
        return FW_EINVAL;
    if (tx_len != 0U && tx == NULL) return FW_EINVAL;
    if (rx_len != 0U && rx == NULL) return FW_EINVAL;
    {
        int result = ops->transfer(dev, addr7, tx, tx_len, rx, rx_len,
                                   timeout_ms);
        if (result < 0) fw_dev_note_error(dev);
        return result;
    }
}

int fw_i2c_mem_read(fw_dev_t *dev, uint16_t addr7, uint16_t mem_addr,
                    uint8_t mem_addr_size, uint8_t *rx, size_t rx_len,
                    uint32_t timeout_ms)
{
    const fw_i2c_ops_t *ops = i2c_ops(dev);
    if (ops == NULL || ops->mem_read == NULL) return FW_ENOTSUP;
    if (!addr_ok(addr7) || rx == NULL || rx_len == 0U ||
        rx_len > FW_I2C_MAX_TRANSFER || !mem_size_ok(mem_addr_size))
        return FW_EINVAL;
    if (mem_addr_size == 1U && mem_addr > 0xFFU) return FW_EINVAL;
    {
        int result = ops->mem_read(dev, addr7, mem_addr, mem_addr_size, rx,
                                   rx_len, timeout_ms);
        if (result < 0) fw_dev_note_error(dev);
        return result;
    }
}

int fw_i2c_mem_write(fw_dev_t *dev, uint16_t addr7, uint16_t mem_addr,
                     uint8_t mem_addr_size, const uint8_t *tx, size_t tx_len,
                     uint32_t timeout_ms)
{
    const fw_i2c_ops_t *ops = i2c_ops(dev);
    if (ops == NULL || ops->mem_write == NULL) return FW_ENOTSUP;
    if (!addr_ok(addr7) || tx == NULL || tx_len == 0U ||
        tx_len > FW_I2C_MAX_TRANSFER || !mem_size_ok(mem_addr_size))
        return FW_EINVAL;
    if (mem_addr_size == 1U && mem_addr > 0xFFU) return FW_EINVAL;
    {
        int result = ops->mem_write(dev, addr7, mem_addr, mem_addr_size, tx,
                                    tx_len, timeout_ms);
        if (result < 0) fw_dev_note_error(dev);
        return result;
    }
}

fw_status_t fw_i2c_probe(fw_dev_t *dev, uint16_t addr7, uint32_t timeout_ms)
{
    const fw_i2c_ops_t *ops = i2c_ops(dev);
    if (ops == NULL || ops->probe == NULL) return FW_ENOTSUP;
    if (!addr_ok(addr7)) return FW_EINVAL;
    {
        int result = ops->probe(dev, addr7, timeout_ms);
        if (result < 0) fw_dev_note_error(dev);
        return (fw_status_t)result;
    }
}
