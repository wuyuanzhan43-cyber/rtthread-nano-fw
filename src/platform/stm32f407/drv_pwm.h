/*
 * drv_pwm.h - STM32 定时器 PWM 实例表驱动接口。
 *
 * 层级：src/platform/stm32f407
 * 职责：接收板级 PWM 描述符，按 FW_USING_PWM4 注册 pwm4（TIM4）。
 * 上下文：configure/set_duty/start/stop 在任务上下文调用，不阻塞。
 * 依赖：fw/pwm.h、stm32f4xx_hal.h。
 *
 * 首版只支持 TIM4 的四路 PWM 输出；不使用中断/DMA/输入捕获。
 */
#ifndef DRV_PWM_H
#define DRV_PWM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "fw/pwm.h"
#include "stm32f4xx_hal.h"

/* 每个定时器最多四路；生成器会校验不超过该值。 */
#define DRV_PWM_MAX_CHANNELS 4

/* 一个 PWM 通道的板级描述；channel 为 1..4。 */
typedef struct {
    uint8_t channel;
    uint8_t port;
    uint8_t pin;
    uint8_t af;
} drv_pwm_channel_t;

/*
 * 板级 PWM 描述符。
 *
 * instance     - 定时器编号；首版固定 4（TIM4）；
 * channel_count- 已接线通道数；
 * channels     - 通道/引脚/AF 表；
 * frequency_hz - 初始频率，来自 Kconfig 的 BOARD_PWM4_PERIOD_HZ。
 */
typedef struct {
    uint8_t instance;
    uint8_t channel_count;
    drv_pwm_channel_t channels[DRV_PWM_MAX_CHANNELS];
    uint32_t frequency_hz;
} drv_pwm_hw_t;

/* 单个 PWM 实例。 */
typedef struct {
    fw_pwm_t base;
    TIM_HandleTypeDef hal;
    uint8_t instance;
    uint16_t channel_mask; /* bit n = 框架通道 n 已配置 */
    uint32_t arr;
    uint32_t duty[DRV_PWM_MAX_CHANNELS];
    bool started;
    fw_drv_t drv;
} drv_pwm_t;

/* 按板级描述符注册 PWM；返回第一个失败实例的错误码。 */
fw_status_t drv_pwm_register_all(const drv_pwm_hw_t *hw, size_t count);

#endif
