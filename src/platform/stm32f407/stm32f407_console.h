/*
 * stm32f407_console.h - RT-Thread 控制台输出适配。
 *
 * 层级：src/platform/stm32f407
 * 职责：把 rt_hw_console_output 绑定到板级选择的串口实例。
 * 上下文：bind 在启动阶段调用；输出可在启动早期/任务/ISR 调用。
 * 依赖：drv_serial.h。
 */
#ifndef STM32F407_CONSOLE_H
#define STM32F407_CONSOLE_H

#include "drv_serial.h"

/*
 * 绑定控制台串口。
 *
 * console 为 NULL 时控制台静默；板级在串口注册后传入
 * drv_serial_instance(BOARD_CONSOLE_UART_NUM)。
 */
void stm32f407_console_bind(drv_serial_t *console);

#endif
