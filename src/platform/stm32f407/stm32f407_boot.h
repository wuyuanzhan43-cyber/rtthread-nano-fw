/*
 * stm32f407_boot.h - STM32F407 平台启动。
 *
 * 层级：src/platform/stm32f407
 * 职责：
 *   1. 捕获复位原因；
 *   2. HAL/NVIC/时钟/SysTick/heap/延时钩子初始化；
 *   3. 向 board 暴露复位原因查询。
 * 上下文：rt_hw_board_init 中、调度器启动前。
 * 依赖：fw/status.h、stm32f407_clock.h、stdint.h。
 */
#ifndef STM32F407_BOOT_H
#define STM32F407_BOOT_H

#include <stdint.h>
#include "fw/status.h"
#include "stm32f407_clock.h"

/* 初始化平台；失败由 board 转成 panic。 */
fw_status_t stm32f407_boot_init(const stm32f407_clock_cfg_t *clock);

/* 上一次复位原因位图（RCC_CSR 标志）。 */
uint32_t stm32f407_reset_cause(void);

/* 复位原因的可读名称；未知返回 "UNKNOWN"。 */
const char *stm32f407_reset_cause_name(uint32_t cause);

#endif
