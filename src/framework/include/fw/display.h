/*
 * fw/display.h - display 设备类（本项目的扩展类）。
 *
 * 层级：src/framework/core
 * 职责：
 *   1. 抽象“矩形窗口 + RGB565 像素流 + 背光”这一组显示动作；
 *   2. 在类层完成参数校验、矩形裁剪和分块填充；
 *   3. 具体面板（ST7735S）只实现窗口设置和像素搬运。
 *
 * 上下文：所有函数都在任务上下文调用，可能阻塞（SPI 传输）。
 * 依赖：fw/dev.h。
 *
 * 为什么单独设计 display 类：
 *   - mr-library 没有统一的 display 抽象，而本项目的 ST7735S 同时需要
 *     SPI 和 GPIO，直接把它当 spi 设备会丢失“窗口/像素/背光”语义；
 *   - 把通用裁剪、分块放进类层，面板驱动只关心命令序列，
 *     以后换 ILI9341 等面板时可以复用同一套上层 API。
 */
#ifndef FW_DISPLAY_H
#define FW_DISPLAY_H

#include "fw/dev.h"

/* ioctl 命令：开关背光。参数为 bool*。 */
#define FW_CTL_DISPLAY_SET_BACKLIGHT (0x08 << 24)

/*
 * 显示信息。
 *
 * width/height     - 逻辑分辨率（像素）；
 * bits_per_pixel   - 当前只支持 16（RGB565），注册时校验。
 */
typedef struct {
    uint16_t width;
    uint16_t height;
    uint8_t bits_per_pixel;
} fw_display_info_t;

/*
 * display 驱动操作集。
 *
 * ctx 是面板驱动私有上下文（例如 st7735s_t*），类层不解释它。
 *
 * init          - 上电初始化序列（复位、命令表、清屏等）；
 * begin_write   - 设置矩形窗口，准备接收像素；
 * write_pixels  - 写入 count 个 RGB565 像素；
 * end_write     - 结束一次窗口写入（片选/背光等收尾）；
 * set_backlight - 控制背光。
 *
 * 所有函数返回 FW_OK 或负错误码。
 */
typedef struct {
    fw_status_t (*init)(void *ctx);
    fw_status_t (*begin_write)(void *ctx, uint16_t x, uint16_t y,
                               uint16_t width, uint16_t height,
                               uint32_t timeout_ms);
    fw_status_t (*write_pixels)(void *ctx, const uint16_t *pixels,
                                size_t count, uint32_t timeout_ms);
    fw_status_t (*end_write)(void *ctx);
    fw_status_t (*set_backlight)(void *ctx, bool on);
} fw_display_ops_t;

/*
 * display 设备对象。
 *
 * dev  - 设备基类，必须是第一个成员；
 * ops  - 面板驱动操作集；
 * ctx  - 面板私有上下文；
 * info - 分辨率与像素格式。
 */
typedef struct {
    fw_dev_t dev;
    const fw_display_ops_t *ops;
    void *ctx;
    fw_display_info_t info;
} fw_display_t;

/*
 * 注册 display 设备。
 * 校验 ops/info 非空、分辨率大于 0、bpp 为 16。
 * 注意：display 没有“驱动 ops”，注册时使用类内部的一个空 fw_drv_t，
 * 具体面板操作通过 display->ops 直接调用。
 */
fw_status_t fw_display_register(fw_display_t *display, const char *name,
                                const fw_display_ops_t *ops, void *ctx,
                                const fw_display_info_t *info);

/* 按名字查找并做类型校验。 */
fw_display_t *fw_display_find(const char *name);

/* 执行面板初始化序列；通常在 main 线程、SPI/GPIO 就绪后调用。 */
fw_status_t fw_display_setup(fw_display_t *display);

/* 用单一颜色填满整个屏幕。 */
fw_status_t fw_display_fill(fw_display_t *display, uint16_t color,
                            uint32_t timeout_ms);

/*
 * 填充矩形。
 * 类层会先裁剪到屏幕范围；完全在屏幕外时直接返回 FW_OK。
 * 内部按 16 像素一块发送，避免在栈上分配大缓冲。
 */
fw_status_t fw_display_fill_rect(fw_display_t *display, uint16_t x, uint16_t y,
                                 uint16_t width, uint16_t height, uint16_t color,
                                 uint32_t timeout_ms);

/*
 * 绘制位图。
 * pixels 是行优先的 RGB565 数组，stride 等于传入的 width。
 * 类层会裁剪矩形，并按行发送；源数据缓冲区由调用者提供。
 */
fw_status_t fw_display_draw_bitmap(fw_display_t *display, uint16_t x, uint16_t y,
                                   uint16_t width, uint16_t height,
                                   const uint16_t *pixels, uint32_t timeout_ms);

/* 通过 ioctl 控制背光。 */
fw_status_t fw_display_set_backlight(fw_display_t *display, bool on);

#endif
