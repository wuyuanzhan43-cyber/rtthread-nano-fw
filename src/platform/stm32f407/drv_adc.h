/*
 * drv_adc.h - STM32 ADC 实例表驱动接口。
 *
 * 层级：src/platform/stm32f407
 * 职责：接收板级 ADC 描述符，按 FW_USING_ADC1 注册 adc1。
 * 上下文：configure/read 在任务上下文调用，阻塞轮询。
 * 依赖：fw/adc.h、stm32f4xx_hal.h。
 *
 * 首版只支持单通道、单次转换、软件触发；不使用中断/DMA。
 */
#ifndef DRV_ADC_H
#define DRV_ADC_H

#include <stddef.h>
#include <stdint.h>
#include "fw/adc.h"
#include "stm32f4xx_hal.h"

/* 板级 ADC 最多配置的通道数；生成器会校验不超过该值。 */
#define DRV_ADC_MAX_CHANNELS 4

/* 一个 ADC 通道的板级描述（模拟引脚没有 AF）。 */
typedef struct {
    uint8_t channel; /* 0..15 */
    uint8_t port;
    uint8_t pin;
} drv_adc_channel_t;

/*
 * 板级 ADC 描述符。
 *
 * channels        - 已接线的通道表；
 * vref/resolution - 电气与精度参数，来自 Kconfig；
 * sample_cycles   - 采样周期，HAL 支持 3/15/28/56/84/112/144/480。
 */
typedef struct {
    uint8_t instance;
    uint8_t channel_count;
    drv_adc_channel_t channels[DRV_ADC_MAX_CHANNELS];
    uint16_t vref_mv;
    uint8_t resolution_bits;
    uint16_t sample_cycles;
} drv_adc_hw_t;

/* 单个 ADC 实例。 */
typedef struct {
    fw_adc_t base;
    ADC_HandleTypeDef hal;
    uint8_t instance;
    uint16_t channel_mask; /* bit n = 通道 n 已配置 */
    uint8_t resolution_bits;
    uint16_t sample_cycles;
    fw_drv_t drv;
} drv_adc_t;

/* 按板级描述符注册 ADC；返回第一个失败实例的错误码。 */
fw_status_t drv_adc_register_all(const drv_adc_hw_t *hw, size_t count);

#endif
