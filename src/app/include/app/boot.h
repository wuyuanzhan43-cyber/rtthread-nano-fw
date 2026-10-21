/*
 * app/boot.h - 应用层 LCD 启动画面。
 *
 * 层级：src/app/boot（公共头位于 src/app/include/app）
 * 职责：在显示设备完成面板初始化后，按 APP_BOOT_MODE 绘制启动画面。
 * 上下文：main 线程、调度器和显示设备就绪之后调用。
 * 依赖：fw/display.h。
 *
 * 设计说明：
 *   启动画面属于应用表现层，不属于 board。board 只注册 lcd 设备并
 *   通过 board_lcd() 暴露基类指针；画什么由 app 决定。
 */
#ifndef APP_BOOT_H
#define APP_BOOT_H

#include "fw/display.h"

/*
 * 绘制启动画面。
 *
 * display 为 NULL（LCD 未启用或未注册）时返回 FW_ENOTSUP，
 * 调用者按“显示可选”处理，不应视为致命错误。
 */
fw_status_t app_boot_display(fw_display_t *display);

#endif
