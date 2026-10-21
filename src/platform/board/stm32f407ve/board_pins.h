/*
 * board_pins.h - STM32F407VE 板级串口/SPI 接线描述与校验。
 *
 * 层级：src/platform/board/stm32f407ve
 * 职责：
 *   1. 把 Kconfig 的 BOARD_* 引脚整理成 platform 驱动描述符；
 *   2. 启动前校验端口、引脚、AF、SWD 保留脚和重复引脚。
 * 上下文：board_devices_init 中、调度器启动前。
 * 依赖：drv_serial.h、drv_spi.h、fw/status.h。
 */
#ifndef BOARD_PINS_H
#define BOARD_PINS_H

#include <stddef.h>
#include "drv_serial.h"
#include "drv_spi.h"
#include "fw/status.h"

/* 返回板级串口描述符表；count 写入条目数。 */
const drv_serial_hw_t *board_serial_hw(size_t *count);

/* 返回板级 SPI 描述符表；count 写入条目数。 */
const drv_spi_hw_t *board_spi_hw(size_t *count);

/* 校验接线；非法组合返回 FW_EINVAL。 */
fw_status_t board_pins_validate(void);

#endif
