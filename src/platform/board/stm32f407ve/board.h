/*
 * board.h - STM32F407VE 板级能力接口。
 *
 * 层级：src/platform/board/stm32f407ve
 * 职责：
 *   1. 声明板级设备注册与串口启动；
 *   2. 向应用暴露控制台、pin、LED、LCD 能力；
 *   3. 暴露板卡名称/版本/复位原因。
 *
 * 上下文：
 *   - board_devices_init/board_serial_start_all 在启动或任务上下文；
 *   - 查询函数可在任务上下文调用，返回静态对象指针。
 *
 * 依赖：fw/dev.h、fw/display.h、fw/status.h、stdbool.h、stdint.h。
 *
 * 分层边界：
 *   board 只描述“这块板子怎么接、什么设备叫什么名字”；
 *   通用启动/console/SysTick/panic 在 platform/stm32f407；
 *   framework 负责设备接口语义。
 */
#ifndef BOARD_H
#define BOARD_H

#include <stdbool.h>
#include <stdint.h>
#include "fw/dev.h"
#include "fw/display.h"
#include "fw/status.h"

/* 注册板级设备；启动前调用，重复调用安全。 */
fw_status_t board_devices_init(void);

/* 打开所有串口的 RX/NVIC 中断；在设备注册之后调用。 */
fw_status_t board_serial_start_all(void);

/* 返回控制台 serial 设备；未注册时返回 NULL。 */
fw_dev_t *board_console_dev(void);

/* 返回唯一的 pin 设备；未初始化时返回 NULL。 */
fw_dev_t *board_pin_dev(void);

/* LED 是否启用。 */
bool board_led_enabled(void);

/* LED 引脚编号（FW_PIN_NUMBER 编码）；未启用返回 -1。 */
int board_led_pin(void);

/* LED 是否高电平点亮。 */
bool board_led_active_high(void);

/* 返回 LCD 对象；未启用或未初始化时返回 NULL。 */
fw_display_t *board_lcd(void);

/* 板卡名称/版本，来自 board_config.h。 */
const char *board_name(void);
const char *board_version(void);

/* 上一次复位原因位图及可读名称。 */
uint32_t board_reset_cause(void);
const char *board_reset_cause_name(uint32_t cause);

/*
 * 看门狗。
 *
 * board_watchdog_start 在所有设备/任务就绪后调用；未启用时返回 FW_OK 且
 * feed 为 no-op。启用后不可停止，main 必须在健康检查通过时周期喂狗。
 */
bool board_watchdog_enabled(void);
fw_status_t board_watchdog_start(void);
void board_watchdog_feed(void);

#endif
