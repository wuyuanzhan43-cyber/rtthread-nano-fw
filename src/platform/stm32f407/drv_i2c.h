/*
 * drv_i2c.h - STM32 I2C 实例表驱动接口。
 *
 * 层级：src/platform/stm32f407
 * 职责：接收板级 I2C 描述符，按 FW_USING_I2Cn 注册 i2c1..i2c3。
 * 上下文：configure/transfer/probe 在任务上下文调用，全部阻塞。
 * 依赖：fw/i2c.h、stm32f4xx_hal.h。
 *
 * 首版只实现主模式阻塞事务，不使用中断/DMA。
 * GPIO 固定为复用开漏（AF_OD）+ 内部上拉；400 kHz 需要外部上拉。
 */
#ifndef DRV_I2C_H
#define DRV_I2C_H

#include <stddef.h>
#include <stdint.h>
#include "fw/i2c.h"
#include "stm32f4xx_hal.h"

/*
 * 板级 I2C 描述符。
 *
 * instance   - I2C 编号 1..3；
 * scl/sda    - 端口 0=A..4=E、引脚号、复用功能号（I2C 固定 AF4）；
 * clock_hz   - 总线频率，来自 Kconfig 的 BOARD_I2Cn_SPEED_HZ。
 */
typedef struct {
    uint8_t instance;
    uint8_t scl_port;
    uint8_t scl_pin;
    uint8_t scl_af;
    uint8_t sda_port;
    uint8_t sda_pin;
    uint8_t sda_af;
    uint32_t clock_hz;
} drv_i2c_hw_t;

/* 单个 I2C 实例。 */
typedef struct {
    fw_i2c_t base;
    I2C_HandleTypeDef hal;
    uint8_t instance;
    fw_drv_t drv;
} drv_i2c_t;

/* 按板级描述符注册 I2C；返回第一个失败实例的错误码。 */
fw_status_t drv_i2c_register_all(const drv_i2c_hw_t *hw, size_t count);

#endif
