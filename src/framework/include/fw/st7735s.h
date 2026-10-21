/*
 * fw/st7735s.h - ST7735S 128x160 SPI TFT 面板驱动。
 *
 * 层级：src/framework/include/fw（实现位于 src/framework/components/display）
 * 职责：
 *   1. 实现 fw_display_ops_t（初始化、窗口、像素、背光）；
 *   2. 通过 fw spi 设备发送命令/数据；
 *   3. 通过 fw pin 设备控制 CS/DC/RES/BL。
 *
 * 上下文：任务上下文；所有操作可能阻塞在 SPI 传输上。
 * 依赖：fw/display.h、fw/spi.h、fw/pin.h。
 *
 * 硬件连接（4 线 SPI）：
 *   SCL/SDA 由 spiN 设备负责；
 *   CS   片选，低有效；
 *   DC   数据/命令选择，0=命令，1=数据；
 *   RES  复位，低有效；
 *   BL   背光，可由 Kconfig 配置为高有效或低有效。
 *
 * 设计说明：
 *   面板驱动不直接操作 GPIO/SPI 寄存器，而是复用 pin/spi 设备；
 *   因此它不依赖 STM32 HAL，也更容易在 Host 上用假设备测试。
 */
#ifndef FW_ST7735S_H
#define FW_ST7735S_H

#include <stdbool.h>
#include <stdint.h>
#include "fw/display.h"
#include "fw/spi.h"
#include "fw/pin.h"

/*
 * 初始化配置。
 *
 * spi/pin      - 已注册的设备指针，必须非空；
 * cs/dc/res/bl - 引脚编号（FW_PIN_NUMBER 编码）；bl < 0 表示无背光控制；
 * bl_active_high - 背光“亮”对应的电平；
 * width/height   - 逻辑分辨率；
 * x_offset/y_offset - 面板可视区域在控制器 RAM 中的偏移，
 *                    不同批次的面板可能需要 0/1/2 像素补偿；
 * madctl         - 扫描方向/颜色顺序寄存器值；
 * invert         - 是否需要反色。
 */
typedef struct {
    fw_dev_t *spi;
    fw_dev_t *pin;
    int cs;
    int dc;
    int res;
    int bl;  /* < 0 表示没有背光 */
    bool bl_active_high;
    uint16_t width;
    uint16_t height;
    uint16_t x_offset;
    uint16_t y_offset;
    uint8_t madctl;
    bool invert;
} st7735s_config_t;

/*
 * 面板对象。
 *
 * base     - display 基类，必须是第一个成员，便于注册和 container_of；
 * spi/pin  - 设备指针副本；
 * cs/dc/res/bl - 引脚编号；
 * bl_active_high - 背光电平；
 * x_offset/y_offset - 窗口偏移；
 * madctl/invert - 面板方向与反色设置；
 * ready    - 初始化序列是否完成；
 * writing  - 是否处于一次 begin_write..end_write 事务中，
 *            用于拒绝嵌套窗口写入。
 */
typedef struct {
    fw_display_t base;
    fw_dev_t *spi;
    fw_dev_t *pin;
    int cs;
    int dc;
    int res;
    int bl;
    bool bl_active_high;
    uint16_t x_offset;
    uint16_t y_offset;
    uint8_t madctl;
    bool invert;
    bool ready;
    bool writing;
} st7735s_t;

/*
 * 注册面板。
 * 只保存配置并注册 display 设备，不会立即发送初始化命令；
 * 实际初始化由 fw_display_setup() 在 main 线程中调用。
 */
fw_status_t st7735s_init(st7735s_t *lcd, const char *name,
                         const st7735s_config_t *config);

/*
 * RGB888 -> RGB565 转换。
 *
 * 取 R 的高 5 位、G 的高 6 位、B 的高 5 位：
 *   bit15..11 = R5
 *   bit10..5  = G6
 *   bit4..0   = B5
 * 这是 ST7735S 在 COLMOD=0x05（16 位色）下的像素格式。
 */
static inline uint16_t st7735s_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((uint16_t)(r & 0xF8U) << 8U) |
                      ((uint16_t)(g & 0xFCU) << 3U) |
                      ((uint16_t)b >> 3U));
}

#endif
