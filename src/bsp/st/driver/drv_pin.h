/*
 * drv_pin.h - STM32 单 pin 设备驱动接口。
 *
 * 层级：src/bsp/st/driver
 * 职责：注册一个名为 "pin" 的设备，覆盖所有 GPIO。
 * 上下文：read/write 只访问寄存器，可在任务或 ISR 中使用；
 *         configure 会调用 HAL_GPIO_Init，只在任务上下文使用。
 * 依赖：fw/pin.h、stm32f4xx_hal.h。
 *
 * 引脚号格式：
 *   高 4 位 = 端口索引（0=A），低 4 位 = 引脚号。
 *   驱动通过 mr_board.h 的端口表和引脚掩码表查表，
 *   不直接依赖 HAL 的 GPIO_PIN_x 数值。
 */
#ifndef DRV_PIN_H
#define DRV_PIN_H

#include "fw/pin.h"

/*
 * 注册 pin 设备。
 *
 * 只能调用一次；重复调用会因设备名 "pin" 已存在而返回 FW_EEXIST。
 * 通常在 board_devices_init() 的最开始调用，因为后续 LED、LCD
 * 以及板级引脚配置都依赖这个设备。
 *
 * 返回 FW_OK 或 core/驱动的错误码。
 */
fw_status_t drv_pin_register(void);

#endif
