/*
 * board.h - STM32F407VE 板级装配接口。
 *
 * 层级：src/board/stm32f407ve
 * 职责：
 *   1. 声明板级初始化、控制台启动、LCD 启动画面等入口；
 *   2. 向应用暴露“板级能力”：控制台设备、pin 设备、LED 引脚、LCD 对象；
 *   3. 声明 panic 入口。
 *
 * 上下文：
 *   - board_devices_init/board_console_start/board_panic 在启动或任务上下文；
 *   - 查询函数可在任务上下文调用，返回静态对象的指针。
 *
 * 依赖：fw/dev.h、fw/display.h、drv_serial.h。
 *
 * 分层边界：
 *   board 负责“这块板子怎么接、什么设备叫什么名字”；
 *   bsp 负责“STM32 外设怎么操作”；
 *   framework 负责“设备接口语义”；
 *   app 只通过本头文件和 fw_dev_* 使用硬件。
 */
#ifndef BOARD_H
#define BOARD_H

#include "fw/dev.h"
#include "fw/display.h"
#include "drv_serial.h"

/*
 * 注册板级设备。
 *
 * 顺序：
 *   1. pin 设备；
 *   2. LED 引脚配置；
 *   3. serial1..N；
 *   4. spi1..N；
 *   5. LCD（依赖 spi 和 pin）。
 *
 * 在 rt_hw_board_init() 中、调度器启动前调用。
 * 重复调用是安全的：内部 devices_ready 标志会直接返回 FW_OK。
 * 返回 FW_OK 或第一个失败设备的错误码。
 */
fw_status_t board_devices_init(void);

/*
 * 打开所有串口中断。
 *
 * 必须在 board_devices_init 之后、应用开始收发之前调用。
 * 当前实现只是转发到 drv_serial_start_all。
 */
fw_status_t board_console_start(void);

/*
 * 绘制 LCD 启动画面。
 *
 * 具体内容由 Kconfig 的 BOARD_LCD_BOOT_MODE 决定：
 *   0 = 四色横条；
 *   1 = 头像图片；
 *   2 = 纯白诊断。
 * LCD 未启用时返回 FW_ENOTSUP。
 */
fw_status_t board_boot_display(void);

/* 返回控制台 serial 设备；设备尚未注册时返回 NULL。 */
fw_dev_t *board_console_dev(void);

/* 返回控制台驱动实例，board_console.c 的轮询输出需要直接访问 USART 寄存器。 */
drv_serial_t *board_console_driver(void);

/* 返回唯一的 pin 设备；未初始化时返回 NULL。 */
fw_dev_t *board_pin_dev(void);

/* 返回 Kconfig 配置的 LED 引脚编号（FW_PIN_NUMBER 编码）。 */
int board_led_pin(void);

/* 返回 LED 是否高电平点亮（Kconfig）。 */
bool board_led_active_high(void);

/* 返回 LCD 对象；未启用或未初始化时返回 NULL。 */
fw_display_t *board_lcd(void);

/*
 * 致命错误停机。
 *
 * 关闭中断，把文件名和行号保存到全局变量（便于调试器查看），
 * 然后进入死循环。不会复位芯片，避免错误被掩盖；现场可用调试器
 * 读取 g_board_panic_file/g_board_panic_line。
 *
 * 该函数也被 HAL 的 assert_param 使用。
 */
void board_panic(const char *file, unsigned line);

#endif
