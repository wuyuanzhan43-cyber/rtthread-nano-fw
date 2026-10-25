/*
 * stm32f407_iwdg.h - 独立看门狗（IWDG）启动与喂狗。
 *
 * 层级：src/platform/stm32f407
 * 职责：按毫秒超时计算 LSI 预分频/重装值，启动后不可停止。
 * 上下文：初始化在任务/启动上下文；喂狗可在任务上下文调用。
 * 依赖：stm32f4xx_hal.h、fw/status.h。
 */
#ifndef STM32F407_IWDG_H
#define STM32F407_IWDG_H

#include "fw/status.h"

/*
 * 启动 IWDG。
 *
 * timeout_ms 使用 LSI 典型值 32kHz 估算，实际超时随芯片 LSI 偏差浮动。
 * 启动后无法关闭；调用者必须保证 main 监督循环会周期喂狗。
 */
fw_status_t stm32f407_iwdg_start(uint32_t timeout_ms);

/* 喂狗；未启动时是安全 no-op。 */
void stm32f407_iwdg_feed(void);

#endif
