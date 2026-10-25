/*
 * fw_spi.c - SPI 设备类的通用实现。
 *
 * 层级：src/framework/core
 * 职责：
 *   1. 校验并保存 SPI 配置；
 *   2. 把 ioctl 转发到驱动的 configure；
 *   3. 把同步 transfer 和异步 async/wait/abort 转发到驱动。
 *
 * 上下文：任务上下文，允许阻塞；wait(FW_NO_WAIT) 不阻塞。
 * 依赖：fw/spi.h。
 *
 * 设计说明：
 *   SPI 类不包含 HAL/RTOS 头文件，也不关心 CPOL/CPHA 如何写寄存器；
 *   它只做参数检查、状态保存和 busy 状态机。实际分频、DMA 和
 *   RT-Thread 信号量全部在 src/platform/stm32f407/drv_spi.c 中完成。
 *
 * busy 状态机：
 *   async 成功启动 -> busy=true；
 *   wait  EAGAIN   -> busy 保持（timeout==0 轮询）；
 *   wait  其他结果 -> busy=false（超时且 abort 失败除外）；
 *   abort 成功     -> busy=false。
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
 *   - busy（异步传输在途）时返回 FW_EBUSY；
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
        if (spi->busy) return FW_EBUSY;
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
 * 异步扩展必须“全有或全无”：只实现一部分会让 wait/abort 的语义
 * 依赖调用顺序，因此在注册期直接拒绝。
 * 注册成功后挂接类 ops、保存配置副本并清 busy。
 */
fw_status_t fw_spi_register(fw_spi_t *spi, const char *name,
                            const fw_drv_t *drv,
                            const fw_spi_config_t *config)
{
    const fw_spi_ops_t *ops;
    bool async_partial;
    fw_status_t result;

    if (spi == NULL || name == NULL || drv == NULL || drv->ops == NULL ||
        config == NULL) return FW_EINVAL;
    if (drv->type != FW_DEV_TYPE_SPI) return FW_EINVAL;

    ops = (const fw_spi_ops_t *)drv->ops;
    async_partial = ops->transfer_async != NULL ||
                    ops->transfer_wait != NULL ||
                    ops->transfer_abort != NULL;
    if (async_partial &&
        (ops->transfer_async == NULL || ops->transfer_wait == NULL ||
         ops->transfer_abort == NULL))
        return FW_EINVAL;

    result = fw_dev_register(&spi->dev, name, FW_DEV_TYPE_SPI, drv);
    if (result != FW_OK) return result;

    spi->dev.ops = &spi_dev_ops;
    spi->config = *config;
    spi->busy = false;
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
 * 有异步传输在途时返回 FW_EBUSY。HAL 的 Size 是 uint16_t，
 * 因此单次长度上限为 FW_SPI_MAX_TRANSFER，避免静默截断。
 *
 * 类不做任何数据拷贝，直接把调用者缓冲区交给驱动，
 * 因此缓冲区必须在该次调用期间保持有效。
 */
int fw_spi_transfer(fw_dev_t *dev, const uint8_t *tx, uint8_t *rx,
                    size_t length, uint32_t timeout_ms)
{
    fw_spi_t *spi;
    const fw_spi_ops_t *ops;
    if (dev == NULL) return FW_EINVAL;
    spi = FW_CONTAINER_OF(dev, fw_spi_t, dev);
    ops = spi_ops(dev);
    if (ops == NULL || ops->transfer == NULL) return FW_ENOTSUP;
    if (spi->busy) return FW_EBUSY;
    if (length == 0U) return 0;
    if (length > FW_SPI_MAX_TRANSFER) return FW_EINVAL;
    if (tx == NULL && rx == NULL) return FW_EINVAL;
    {
        int result = ops->transfer(dev, tx, rx, length, timeout_ms);
        if (result < 0) fw_dev_note_error(dev);
        return result;
    }
}

/*
 * 启动异步传输。
 *
 * 先把 busy 置位再调用驱动：驱动失败时立即回滚。
 * 驱动返回非负值时保持 busy，直到 wait/abort 结束本次传输。
 */
int fw_spi_transfer_async(fw_dev_t *dev, const uint8_t *tx, uint8_t *rx,
                          size_t length)
{
    fw_spi_t *spi;
    const fw_spi_ops_t *ops;
    int result;

    if (dev == NULL) return FW_EINVAL;
    spi = FW_CONTAINER_OF(dev, fw_spi_t, dev);
    ops = spi_ops(dev);
    if (ops == NULL || ops->transfer_async == NULL) return FW_ENOTSUP;
    if (length == 0U) return 0;
    if (length > FW_SPI_MAX_TRANSFER) return FW_EINVAL;
    if (tx == NULL && rx == NULL) return FW_EINVAL;
    if (spi->busy) return FW_EBUSY;

    spi->busy = true;
    result = ops->transfer_async(dev, tx, rx, length);
    if (result < 0) spi->busy = false;
    return result;
}

/*
 * 等待异步传输结束。
 *
 * 超时处理放在类层：驱动只报告 FW_ETIMEOUT，类层负责 abort 并决定
 * busy 是否清除。这样驱动不必依赖类层状态，也保证“超时后总线仍忙”
 * 不会让下一次 transfer 悄悄覆盖在途数据。
 */
fw_status_t fw_spi_transfer_wait(fw_dev_t *dev, uint32_t timeout_ms)
{
    fw_spi_t *spi;
    const fw_spi_ops_t *ops;
    int result;

    if (dev == NULL) return FW_EINVAL;
    spi = FW_CONTAINER_OF(dev, fw_spi_t, dev);
    ops = spi_ops(dev);
    if (ops == NULL || ops->transfer_wait == NULL) return FW_ENOTSUP;
    if (!spi->busy) return FW_EINVAL;

    result = ops->transfer_wait(dev, timeout_ms);
    if (result == FW_EAGAIN) return FW_EAGAIN; /* 轮询未完成，保持 busy */
    if (result < 0) fw_dev_note_error(dev);

    if (result == FW_ETIMEOUT) {
        int abort_result = ops->transfer_abort != NULL
                               ? ops->transfer_abort(dev)
                               : FW_ENOTSUP;
        if (abort_result != FW_OK) return FW_EIO; /* 清理失败，保持 busy */
        spi->busy = false;
        return FW_ETIMEOUT;
    }

    /*
     * 驱动用 FW_EBUSY 表示“传输已结束但清理未完成”，例如 HAL 的
     * 另一条 DMA stream 仍处于 ABORT。此时不能清 busy，否则类层的
     * abort API 会因 !busy 直接返回，调用者失去重试清理的入口。
     */
    if (result == FW_EBUSY) return FW_EIO;
    spi->busy = false;
    return (fw_status_t)result;
}

/*
 * 取消在途异步传输。
 *
 * 未忙时幂等成功；abort 失败时保持 busy，允许调用者重试。
 */
fw_status_t fw_spi_transfer_abort(fw_dev_t *dev)
{
    fw_spi_t *spi;
    const fw_spi_ops_t *ops;
    int result;

    if (dev == NULL) return FW_EINVAL;
    spi = FW_CONTAINER_OF(dev, fw_spi_t, dev);
    if (!spi->busy) return FW_OK;
    ops = spi_ops(dev);
    if (ops == NULL || ops->transfer_abort == NULL) return FW_ENOTSUP;

    result = ops->transfer_abort(dev);
    if (result == FW_OK) spi->busy = false;
    return (fw_status_t)result;
}
