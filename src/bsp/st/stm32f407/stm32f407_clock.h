/*
 * stm32f407_clock.h - STM32F407 时钟树初始化接口。
 *
 * 层级：src/bsp/st/stm32f407
 * 职责：按 Kconfig 选择 HSI 16 MHz 或 HSE 8 MHz -> PLL -> 168 MHz。
 * 上下文：rt_hw_board_init 中、调度器启动前调用。
 * 依赖：fw/status.h。
 *
 * 两种配置：
 *   BOARD_CLOCK_HSI_16    - 内部 RC 16MHz，无需外部晶振，SYSCLK=16MHz；
 *   BOARD_CLOCK_HSE_8_168 - 外部 8MHz 晶振，PLL 到 168MHz。
 *
 * 选择 HSI 是为了“板子没焊晶振也能跑”；选择 HSE 是为了
 * 更高的主频和更准确的波特率。两者由 Kconfig 互斥选择。
 */
#ifndef STM32F407_CLOCK_H
#define STM32F407_CLOCK_H

#include "fw/status.h"

/*
 * 初始化系统时钟。
 *
 * 成功返回 FW_OK；HAL 配置失败返回 FW_EIO。
 * 失败时 board_panic 会记录文件和行号并停机，
 * 因为时钟错误会导致串口波特率、SysTick 全部不可信。
 *
 * 调用前 HAL_Init() 已经执行；调用后 SystemCoreClock 已更新，
 * 可以据此配置 SysTick 和串口。
 */
fw_status_t stm32f407_clock_init(void);

#endif
