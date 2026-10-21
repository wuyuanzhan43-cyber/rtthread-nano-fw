/*
 * board_clock.h - STM32F407VE 板级时钟描述。
 *
 * 层级：src/platform/board/stm32f407ve
 * 职责：把 Kconfig 的板级时钟选择转换成 stm32f407_clock_cfg_t。
 * 上下文：启动阶段、调度器启动前。
 * 依赖：stm32f407_clock.h。
 */
#ifndef BOARD_CLOCK_H
#define BOARD_CLOCK_H

#include "stm32f407_clock.h"

/* 返回板级时钟配置；由 board.c 传给 stm32f407_boot_init。 */
const stm32f407_clock_cfg_t *board_clock_config(void);

#endif
