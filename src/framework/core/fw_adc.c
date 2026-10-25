/*
 * fw_adc.c - ADC 设备类的通用实现。
 *
 * 层级：src/framework/core
 * 职责：
 *   1. 校验通道、分辨率和采样周期；
 *   2. 把 read/ioctl 转发到驱动；
 *   3. 提供原始采样与 mV 换算 helper。
 *
 * 上下文：任务上下文，阻塞。
 * 依赖：fw/adc.h。
 *
 * 设计说明：
 *   类层不包含 HAL；通道号在类层保持 0..15，驱动负责映射到
 *   ADC_CHANNEL_n 并处理采样时间。一个设备覆盖所有通道。
 */
#include "fw/adc.h"
#include <string.h>

static const fw_adc_ops_t *adc_ops(const fw_dev_t *dev)
{
    if (dev == NULL || dev->drv == NULL || dev->drv->ops == NULL) return NULL;
    return (const fw_adc_ops_t *)dev->drv->ops;
}

/* F407 外部通道为 0..15；内部通道不在首版范围。 */
static bool channel_ok(int channel)
{
    return channel >= 0 && channel <= 15;
}

/* HAL 支持的采样周期集合。 */
static bool sample_cycles_ok(uint16_t cycles)
{
    switch (cycles) {
    case 3U:
    case 15U:
    case 28U:
    case 56U:
    case 84U:
    case 112U:
    case 144U:
    case 480U:
        return true;
    default:
        return false;
    }
}

static bool config_ok(const fw_adc_config_t *config)
{
    if (config == NULL || config->vref_mv == 0U) return false;
    if (config->resolution_bits != 8U && config->resolution_bits != 10U &&
        config->resolution_bits != 12U) return false;
    return sample_cycles_ok(config->sample_cycles);
}

/*
 * 设备 read 入口：off 是通道号，buf 至少 2 字节。
 * 采样值按主机字节序写入，与 uint16_t 内存表示一致。
 */
static int adc_read(fw_dev_t *dev, int off, void *buf, size_t size,
                    uint32_t timeout_ms)
{
    const fw_adc_ops_t *ops = adc_ops(dev);
    uint16_t raw = 0U;
    int result;

    if (ops == NULL || ops->read == NULL) return FW_ENOTSUP;
    if (buf == NULL || size < sizeof(raw) || !channel_ok(off))
        return FW_EINVAL;

    result = ops->read(dev, off, &raw, timeout_ms);
    if (result < 0) {
        fw_dev_note_error(dev);
        return result;
    }
    memcpy(buf, &raw, sizeof(raw));
    return (int)sizeof(raw);
}

/* SET_CONFIG 校验 vref/分辨率/采样周期；GET_CONFIG 返回副本。 */
static int adc_ioctl(fw_dev_t *dev, int off, int cmd, void *args)
{
    fw_adc_t *adc = FW_CONTAINER_OF(dev, fw_adc_t, dev);
    const fw_adc_ops_t *ops = adc_ops(dev);
    (void)off;

    if (cmd == FW_CTL_ADC_SET_CONFIG) {
        fw_adc_config_t config;
        if (args == NULL || ops == NULL || ops->configure == NULL)
            return FW_EINVAL;
        config = *(const fw_adc_config_t *)args;
        if (!config_ok(&config)) return FW_EINVAL;
        {
            int result = ops->configure(dev, &config);
            if (result < 0) return result;
        }
        adc->config = config;
        return (int)sizeof(config);
    }

    if (cmd == FW_CTL_ADC_GET_CONFIG) {
        if (args == NULL) return FW_EINVAL;
        *(fw_adc_config_t *)args = adc->config;
        return (int)sizeof(adc->config);
    }
    return FW_ENOTSUP;
}

static const fw_dev_ops_t adc_dev_ops = {
    NULL,
    NULL,
    adc_read,
    NULL,
    adc_ioctl,
    NULL
};

fw_status_t fw_adc_register(fw_adc_t *adc, const char *name,
                            const fw_drv_t *drv,
                            const fw_adc_config_t *config)
{
    const fw_adc_ops_t *ops;
    fw_status_t result;

    if (adc == NULL || name == NULL || drv == NULL || drv->ops == NULL ||
        !config_ok(config)) return FW_EINVAL;
    if (drv->type != FW_DEV_TYPE_ADC) return FW_EINVAL;
    ops = (const fw_adc_ops_t *)drv->ops;
    if (ops->configure == NULL || ops->read == NULL) return FW_EINVAL;

    result = fw_dev_register(&adc->dev, name, FW_DEV_TYPE_ADC, drv);
    if (result != FW_OK) return result;

    adc->dev.ops = &adc_dev_ops;
    adc->config = *config;
    return FW_OK;
}

fw_adc_t *fw_adc_find(const char *name)
{
    fw_dev_t *dev = fw_dev_find(name);
    if (dev == NULL || dev->type != FW_DEV_TYPE_ADC) return NULL;
    return FW_CONTAINER_OF(dev, fw_adc_t, dev);
}

fw_status_t fw_adc_read(fw_dev_t *dev, int channel, uint16_t *raw,
                        uint32_t timeout_ms)
{
    const fw_adc_ops_t *ops = adc_ops(dev);
    if (ops == NULL || ops->read == NULL) return FW_ENOTSUP;
    if (raw == NULL || !channel_ok(channel)) return FW_EINVAL;
    {
        int result = ops->read(dev, channel, raw, timeout_ms);
        if (result < 0) fw_dev_note_error(dev);
        return (fw_status_t)result;
    }
}

fw_status_t fw_adc_read_mv(fw_dev_t *dev, int channel, uint32_t *mv,
                           uint32_t timeout_ms)
{
    fw_adc_t *adc;
    uint16_t raw = 0U;
    uint32_t max;
    fw_status_t result;

    if (dev == NULL || mv == NULL) return FW_EINVAL;
    adc = FW_CONTAINER_OF(dev, fw_adc_t, dev);
    if (adc->config.vref_mv == 0U || adc->config.resolution_bits < 8U ||
        adc->config.resolution_bits > 12U)
        return FW_EINVAL;

    result = fw_adc_read(dev, channel, &raw, timeout_ms);
    if (result != FW_OK) return result;

    max = (1UL << adc->config.resolution_bits) - 1UL;
    *mv = ((uint32_t)raw * adc->config.vref_mv + max / 2U) / max;
    return FW_OK;
}
