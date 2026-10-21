/*
 * drv_spi.h - STM32 SPI 实例表驱动接口。
 *
 * 层级：src/platform/stm32f407
 * 职责：接收板级 SPI 描述符，按 FW_USING_SPIn 注册 spi1..N。
 * 上下文：configure/transfer 在任务上下文调用，允许阻塞。
 * 依赖：fw/spi.h、stm32f4xx_hal.h。
 *
 * 设计说明：
 *   SPI 是同步总线，没有 RX/TX ring 和中断事件；
 *   一次 transfer 在 HAL 中轮询完成。
 *   片选由具体外设（如 ST7735S）用 pin 设备控制，驱动不管 CS。
 */
#ifndef DRV_SPI_H
#define DRV_SPI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "fw/spi.h"
#include "stm32f4xx_hal.h"

/*
 * 板级 SPI 描述符。
 *
 * instance - SPI 编号 1..3；
 * sck/mosi/miso - 端口 0=A..4=E、引脚号、复用功能号；
 * use_miso - false 时配置为 1LINE，只允许发送；
 * max_hz   - 初始频率上限。
 */
typedef struct {
    uint8_t instance;
    uint8_t sck_port;
    uint8_t sck_pin;
    uint8_t sck_af;
    uint8_t mosi_port;
    uint8_t mosi_pin;
    uint8_t mosi_af;
    uint8_t miso_port;
    uint8_t miso_pin;
    uint8_t miso_af;
    bool use_miso;
    uint32_t max_hz;
} drv_spi_hw_t;

/*
 * 单个 SPI 实例。
 *
 * base     - SPI 类对象，必须放在第一个成员；
 * hal      - HAL 句柄；
 * pclk     - 所在总线时钟；
 * instance - 实例编号 1..3；
 * use_miso - 是否双线；false 时拒绝 rx 传输；
 * drv      - 驱动描述。
 */
typedef struct {
    fw_spi_t base;
    SPI_HandleTypeDef hal;
    uint32_t pclk;
    uint8_t instance;
    bool use_miso;
    fw_drv_t drv;
} drv_spi_t;

/* 按板级描述符注册 SPI；返回第一个失败实例的错误码。 */
fw_status_t drv_spi_register_all(const drv_spi_hw_t *hw, size_t count);

#endif
