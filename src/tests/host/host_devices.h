/*
 * host_devices.h - Host 测试用的假 serial/pin/display 设备。
 *
 * 层级：src/tests/host
 * 职责：
 *   1. 在没有 RTOS/HAL 的 PC 上实现 fw serial/pin/display 驱动 ops；
 *   2. 记录应用产生的输出（发送字节、引脚电平、像素），供断言使用；
 *   3. 提供可手动推进的毫秒时钟，避免真实延时。
 *
 * 上下文：单线程测试，不需要锁。
 * 依赖：fw/clock.h、fw/dev.h、fw/display.h、fw/pin.h、fw/serial.h。
 *
 * 设计说明：
 *   假设备也走完整的 mr 风格设备核心（注册、open、read/write/ioctl），
 *   而不是直接调用 demo_app 内部函数。这样测试覆盖的是真实的
 *   framework 行为，只有最底层的 HAL 被替换掉。
 *
 * 所有可观察状态都集中在 fw_host_t 中：
 *   - tx[]/tx_length：串口实际发出的字节；
 *   - pin_level/pin_writes：最后一个引脚电平和写次数；
 *   - display_*：窗口、像素、背光记录；
 *   - now_ms：假时钟。
 */
#ifndef FW_HOST_DEVICES_H
#define FW_HOST_DEVICES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "fw/clock.h"
#include "fw/dev.h"
#include "fw/display.h"
#include "fw/pin.h"
#include "fw/serial.h"

/* 假串口 ring 和发送记录的容量；足够容纳测试中的所有帧。 */
#define HOST_SERIAL_CAPACITY 1024U

/* 假 LCD 的分辨率，与 ST7735S 默认配置一致。 */
#define HOST_DISPLAY_WIDTH 128U
#define HOST_DISPLAY_HEIGHT 160U

/* 假 LED 使用的引脚号；pin 假设备忽略具体编号。 */
#define HOST_LED_PIN 0

/*
 * Host 测试环境。
 *
 * 时钟：
 *   now_ms           - 可由 host_advance 手动推进；
 *   delay_ms_total   - 记录 fw_delay_ms 被要求延时多少毫秒；
 *   delay_calls      - 延时调用次数。
 *
 * 串口：
 *   rx_storage/wr_storage - 供 serial ring 使用的静态存储；
 *   tx[]/tx_length        - 驱动“发送”出去的字节记录；
 *   write_limit           - 当前 TX ring 容量，用于模拟短写/背压；
 *   serial_errors         - 保留的诊断计数。
 *
 * 引脚：
 *   pin_level      - 最后一次写入的电平；
 *   pin_writes     - 写次数；
 *   pin_configures - 配置次数。
 *
 * 显示：
 *   display_writing - 是否处于 begin..end 之间；
 *   display_x/y/width/height - 最后一次窗口；
 *   display_pixels  - 收到的像素序列；
 *   display_pixel_count - 已收像素数；
 *   display_begins/ends - 事务次数；
 *   backlight/backlight_changes - 背光状态与变化次数。
 *
 * 设备对象：
 *   serial/pin/display - 注册到设备表的静态对象，必须放在最后，
 *                        因为 fake ops 会用 FW_CONTAINER_OF 从 dev
 *                        反推 fw_host_t（dev 必须是第一个成员）。
 */
typedef struct {
    uint32_t now_ms;
    uint32_t delay_ms_total;
    unsigned delay_calls;

    uint8_t rx_storage[HOST_SERIAL_CAPACITY];
    uint8_t wr_storage[HOST_SERIAL_CAPACITY];
    uint8_t tx[HOST_SERIAL_CAPACITY];
    size_t tx_length;
    size_t write_limit;
    unsigned serial_errors;

    bool pin_level;
    unsigned pin_writes;
    unsigned pin_configures;

    bool display_writing;
    uint16_t display_x;
    uint16_t display_y;
    uint16_t display_width;
    uint16_t display_height;
    uint16_t display_pixels[HOST_DISPLAY_WIDTH * HOST_DISPLAY_HEIGHT];
    size_t display_pixel_count;
    unsigned display_begins;
    unsigned display_ends;
    bool backlight;
    unsigned backlight_changes;

    fw_serial_t serial;
    fw_pin_t pin;
    fw_display_t display;
} fw_host_t;

/*
 * 初始化测试环境：
 *   - 清零所有记录；
 *   - 复位设备注册表；
 *   - 安装假延时钩子。
 * 不注册设备；需要时再调用 host_register_devices。
 */
void host_init(fw_host_t *host);

/*
 * 注册 serial1、pin、lcd 三个假设备。
 * 必须在 host_init 之后调用；重复注册同名设备会失败。
 */
void host_register_devices(fw_host_t *host);

/* 只复位环境和设备表，不安装延时钩子（host_init 内部使用）。 */
void host_reset(fw_host_t *host);

/* 返回由 host->now_ms 驱动的 fw_clock_t。 */
fw_clock_t host_clock(fw_host_t *host);

/* 推进假时钟，用于触发周期上报、半帧超时等时间相关逻辑。 */
void host_advance(fw_host_t *host, uint32_t ms);

/*
 * 向假串口注入接收数据（直接写入 serial 的 rd_fifo）。
 * 返回实际写入字节数；ring 满时可能短写。
 */
size_t host_serial_feed(fw_host_t *host, const uint8_t *data, size_t length);

/* 查询假串口 RX ring 中还有多少字节未读。 */
size_t host_serial_rx_available(const fw_host_t *host);

/*
 * 调整 TX ring 容量，用于模拟“应用写得比硬件快”的背压场景。
 * limit=0 会被当作 1；上限为 HOST_SERIAL_CAPACITY。
 * 会重新初始化 wr_fifo，已有未发送数据会被丢弃。
 */
void host_serial_set_write_limit(fw_host_t *host, size_t limit);

#endif
