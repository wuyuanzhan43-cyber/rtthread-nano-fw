/*
 * fw/adc.h - mr 风格 ADC 设备类。
 *
 * 层级：src/framework/core
 * 职责：
 *   1. 每个 ADC 控制器对应一个设备：adc1..adcN；
 *   2. 保存参考电压/分辨率/采样周期配置；
 *   3. 提供单通道阻塞采样和 mV 换算。
 *
 * 上下文：阻塞轮询，必须在任务上下文使用。
 * 依赖：fw/dev.h。
 *
 * 通道语义：
 *   - fw_dev_read 的 off 是 ADC 通道号（0..15）；
 *   - 一个 ADC 设备覆盖所有通道，不为每个通道单独注册设备。
 */
#ifndef FW_ADC_H
#define FW_ADC_H

#include "fw/dev.h"

/* ioctl 命令：设置/读取 ADC 配置。参数均为 fw_adc_config_t*。 */
#define FW_CTL_ADC_SET_CONFIG (0x08 << 24)
#define FW_CTL_ADC_GET_CONFIG (-(0x08 << 24))

/*
 * ADC 配置。
 *
 * vref_mv         - 参考电压（毫伏），必须大于 0；
 * resolution_bits - 8/10/12；F407 的 HAL 只支持这三种；
 * sample_cycles   - 采样周期，必须是 3/15/28/56/84/112/144/480 之一。
 */
typedef struct {
    uint16_t vref_mv;
    uint8_t resolution_bits;
    uint16_t sample_cycles;
} fw_adc_config_t;

/*
 * ADC 驱动操作集。
 *
 * configure - 应用分辨率/时钟分频等全局配置；
 * read      - 对指定通道做一次阻塞转换，写入原始值。
 */
typedef struct {
    int (*configure)(fw_dev_t *dev, const fw_adc_config_t *config);
    int (*read)(fw_dev_t *dev, int channel, uint16_t *raw,
                uint32_t timeout_ms);
} fw_adc_ops_t;

/* ADC 设备对象；dev 必须是第一个成员。 */
typedef struct {
    fw_dev_t dev;
    fw_adc_config_t config;
} fw_adc_t;

/* 注册 ADC 设备；drv->type 必须为 FW_DEV_TYPE_ADC。 */
fw_status_t fw_adc_register(fw_adc_t *adc, const char *name,
                            const fw_drv_t *drv,
                            const fw_adc_config_t *config);

/* 按名字查找并做类型校验。 */
fw_adc_t *fw_adc_find(const char *name);

/*
 * 单通道采样。
 * channel 为 0..15；raw 不能为空。返回 FW_OK 或负错误码。
 */
fw_status_t fw_adc_read(fw_dev_t *dev, int channel, uint16_t *raw,
                        uint32_t timeout_ms);

/* 采样并换算成毫伏，四舍五入。 */
fw_status_t fw_adc_read_mv(fw_dev_t *dev, int channel, uint32_t *mv,
                           uint32_t timeout_ms);

#endif
