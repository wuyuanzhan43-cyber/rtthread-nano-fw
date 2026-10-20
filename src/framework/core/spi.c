/*
 * spi.c - SPI 设备类的通用实现。
 *
 * 层级：src/framework/core
 * 职责：
 *   1. 校验并保存 SPI 配置；
 *   2. 把 ioctl 转发到驱动的 configure；
 *   3. 把 transfer 转发到驱动的同步传输函数。
 *
 * 上下文：任务上下文，允许阻塞。
 * 依赖：fw/spi.h。
 *
 * 设计说明：
 *   SPI 类不包含 HAL 头文件，也不关心 CPOL/CPHA 如何写寄存器；
 *   它只做参数检查和状态保存。实际分频、时钟使能和 GPIO 复用
 *   全部在 bsp/st/driver/drv_spi.c 中完成。
 */
#include "fw/spi.h"

/* 从基类取驱动 ops，任何一层为空都返回 NULL。 */
static const fw_spi_ops_t *spi_ops(const fw_dev_t *dev)
{
    if (dev == NULL || dev->drv == NULL || dev->drv->ops == NULL) return NULL;
    return (const fw_spi_ops_t *)dev->drv->ops;
}

/*
 * SPI 控制接口。
 *
 * SET_CONFIG:
 *   - 校验 max_hz > 0、mode 在 0..3、bit_order 为 0/1；
 *   - 先调用驱动 configure，成功后才更新软件副本；
 *   - 返回 sizeof(config)。
 *
 * GET_CONFIG:
 *   - 直接返回软件副本，不访问硬件。
 *
 * 其他命令返回 FW_ENOTSUP。
 */
static int spi_ioctl(fw_dev_t *dev, int off, int cmd, void *args)
{
    fw_spi_t *spi = FW_CONTAINER_OF(dev, fw_spi_t, dev);
    const fw_spi_ops_t *ops = spi_ops(dev);
    (void)off; /* SPI 没有通道/偏移概念，整个总线共用一个配置 */

    if (cmd == FW_CTL_SPI_SET_CONFIG) {
        fw_spi_config_t config;
        if (args == NULL || ops == NULL || ops->configure == NULL)
            return FW_EINVAL;
        config = *(const fw_spi_config_t *)args;

        /* 提前拒绝驱动无法表达的配置，避免“返回成功但硬件没变”。 */
        if (config.max_hz == 0U || config.mode < 0 || config.mode > 3 ||
            (config.bit_order != 0 && config.bit_order != 1)) return FW_EINVAL;
        {
            int result = ops->configure(dev, &config);
            if (result < 0) return result;
        }
        spi->config = config;
        return (int)sizeof(config);
    }

    if (cmd == FW_CTL_SPI_GET_CONFIG) {
        if (args == NULL) return FW_EINVAL;
        *(fw_spi_config_t *)args = spi->config;
        return (int)sizeof(spi->config);
    }
    return FW_ENOTSUP;
}

/*
 * SPI 类操作表。
 * 只有 ioctl 有实现：配置通过 ioctl 完成，数据通过 fw_spi_transfer 完成。
 */
static const fw_dev_ops_t spi_dev_ops = {
    NULL,
    NULL,
    NULL,
    NULL,
    spi_ioctl,
    NULL
};

/*
 * 注册 SPI 设备。
 *
 * 校验对象、名字、驱动和初始配置；drv->type 必须为 SPI。
 * 注册成功后挂接类 ops 并保存配置副本。
 */
fw_status_t fw_spi_register(fw_spi_t *spi, const char *name,
                            const fw_drv_t *drv,
                            const fw_spi_config_t *config)
{
    fw_status_t result;
    if (spi == NULL || name == NULL || drv == NULL || config == NULL)
        return FW_EINVAL;
    if (drv->type != FW_DEV_TYPE_SPI) return FW_EINVAL;

    result = fw_dev_register(&spi->dev, name, FW_DEV_TYPE_SPI, drv);
    if (result != FW_OK) return result;

    spi->dev.ops = &spi_dev_ops;
    spi->config = *config;
    return FW_OK;
}

/* 查找并做类型校验，返回可安全转换为 fw_spi_t* 的指针。 */
fw_spi_t *fw_spi_find(const char *name)
{
    fw_dev_t *dev = fw_dev_find(name);
    if (dev == NULL || dev->type != FW_DEV_TYPE_SPI) return NULL;
    return FW_CONTAINER_OF(dev, fw_spi_t, dev);
}

/*
 * 同步传输。
 *
 * 参数组合：
 *   tx != NULL, rx != NULL -> 全双工；
 *   tx != NULL, rx == NULL -> 只发；
 *   tx == NULL, rx != NULL -> 只收；
 *   两者都为空             -> FW_EINVAL。
 *
 * 类不做任何数据拷贝，直接把调用者缓冲区交给驱动，
 * 因此缓冲区必须在该次调用期间保持有效。
 */
int fw_spi_transfer(fw_dev_t *dev, const uint8_t *tx, uint8_t *rx,
                    size_t length, uint32_t timeout_ms)
{
    const fw_spi_ops_t *ops = spi_ops(dev);
    if (ops == NULL || ops->transfer == NULL) return FW_ENOTSUP;
    if (length == 0U) return 0;
    if (tx == NULL && rx == NULL) return FW_EINVAL;
    return ops->transfer(dev, tx, rx, length, timeout_ms);
}
