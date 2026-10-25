/*
 * stm32f407_nvm.h - 参数存储的 STM32F407 Flash 后端。
 *
 * 层级：src/platform/stm32f407
 * 职责：为 fw_param 提供 read/program/erase 三个后端操作。
 * 上下文：任务上下文；擦除/编程会阻塞。
 * 依赖：fw/param.h、board_config.h。
 *
 * 分区要求：
 *   - BOARD_NVM_BASE 位于 Flash 尾部，slot 数固定为 2；
 *   - 每个 slot 必须是一个可独立擦除的扇区（slot_kb == sector_kb）。
 */
#ifndef STM32F407_NVM_H
#define STM32F407_NVM_H

#include "fw/param.h"

/* 成功返回 FW_OK；板级未启用 NVM 返回 FW_ENOTSUP。 */
fw_status_t stm32f407_nvm_init(void);

/* 返回后端指针；未启用/未初始化返回 NULL。 */
const fw_param_backend_t *stm32f407_nvm_backend(void);

#endif
