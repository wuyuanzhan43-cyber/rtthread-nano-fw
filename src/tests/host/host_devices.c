/*
 * host_devices.c - Host 假设备实现。
 *
 * 层级：src/tests/host
 * 职责：
 *   1. 实现 fw_serial_ops_t / fw_pin_ops_t / fw_display_ops_t；
 *   2. 把驱动侧行为记录到 fw_host_t，供测试断言；
 *   3. 让 framework 的注册、open、read/write/ioctl 路径在不依赖
 *      RTOS/HAL 的情况下完整运行。
 *
 * 上下文：单线程测试。
 * 依赖：host_devices.h、string.h。
 *
 * 关键模拟：
 *   - 串口“发送”不是异步的：start_tx 会把 TX ring 一次性搬空，
 *     相当于一个立即完成的 TXE 中断循环；
 *   - 串口“等待”不会真的阻塞：有数据/错误立即返回，
 *     没有则根据 timeout 返回 EAGAIN/ETIMEOUT；
 *   - 显示把窗口和像素记录到数组，便于检查裁剪/分块结果；
 *   - pin 只记录最后一个电平和写次数，不区分具体编号。
 */
#include "host_devices.h"
#include <string.h>

/* 假时钟：直接返回 host->now_ms，由测试手动推进。 */
static uint32_t host_now(void *ctx)
{
    return ((fw_host_t *)ctx)->now_ms;
}

/* 假延时：不真正等待，避免测试变慢；调用次数由 host_delay 记录不到，
 * 因为 fw_delay_ms 只传 ms。当前测试不依赖延时统计。 */
static void host_delay(uint32_t ms)
{
    (void)ms;
}

/* 串口配置：假设备没有硬件，直接成功。 */
static int fake_serial_configure(fw_dev_t *dev, const fw_serial_config_t *cfg)
{
    (void)dev;
    (void)cfg;
    return FW_OK;
}

/* 串口读字节：假设备不使用该路径（ISR 由 start_tx/feed 模拟）。 */
static int fake_serial_read_byte(fw_dev_t *dev)
{
    (void)dev;
    return 0;
}

/*
 * 串口写字节：把字节追加到 host->tx。
 *
 * 这就是“硬件发送”的终点；数组满时返回 FW_EOVERFLOW，
 * 与真实驱动写失败的行为类似。
 */
static int fake_serial_write_byte(fw_dev_t *dev, uint8_t byte)
{
    fw_host_t *host = FW_CONTAINER_OF(dev, fw_host_t, serial.dev);
    if (host->tx_length >= sizeof(host->tx)) return FW_EOVERFLOW;
    host->tx[host->tx_length++] = byte;
    return 0;
}

/*
 * 串口启动发送：模拟 TXE 中断循环。
 *
 * 真实驱动只打开 TXE 中断，随后每个中断发送一个字节；
 * 假设备直接循环调用类的 WR 事件，直到 wr_fifo 为空。
 * 这样 fw_dev_write 返回时，host->tx 中就已经有完整数据，
 * 测试不需要模拟中断时序。
 */
static int fake_serial_start_tx(fw_dev_t *dev)
{
    fw_host_t *host = FW_CONTAINER_OF(dev, fw_host_t, serial.dev);
    while (host->serial.wr_fifo.count != 0U) {
        if (fw_dev_isr(dev, FW_ISR_SERIAL_WR, NULL) < 0) break;
    }
    return 0;
}

/* 停止发送：假设备没有中断需要关闭。 */
static int fake_serial_stop_tx(fw_dev_t *dev)
{
    (void)dev;
    return 0;
}

/*
 * 等待接收：不阻塞。
 *   - 已有数据或错误 -> FW_OK；
 *   - 非阻塞且无数据 -> FW_EAGAIN；
 *   - 带超时但无数据 -> FW_ETIMEOUT（测试中不会真的等待）。
 */
static fw_status_t fake_serial_wait_rx(fw_dev_t *dev, uint32_t timeout_ms)
{
    fw_host_t *host = FW_CONTAINER_OF(dev, fw_host_t, serial.dev);
    if (host->serial.error != FW_OK || host->serial.rd_fifo.count != 0U)
        return FW_OK;
    return timeout_ms == 0U ? FW_EAGAIN : FW_ETIMEOUT;
}

/* 假临界区：单线程测试，无需关中断。 */
static int fake_serial_lock(fw_dev_t *dev)
{
    (void)dev;
    return 0;
}

static void fake_serial_unlock(fw_dev_t *dev, int level)
{
    (void)dev;
    (void)level;
}

/* 串口驱动 ops 表，字段顺序与 fw_serial_ops_t 一致。 */
static const fw_serial_ops_t fake_serial_ops = {
    fake_serial_configure,
    fake_serial_read_byte,
    fake_serial_write_byte,
    fake_serial_start_tx,
    fake_serial_stop_tx,
    fake_serial_wait_rx,
    fake_serial_lock,
    fake_serial_unlock
};

static const fw_drv_t fake_serial_drv = {
    FW_DEV_TYPE_SERIAL, &fake_serial_ops, NULL
};

/* 引脚配置：只记录调用次数；模式/编号在假设备中不关心。 */
static int fake_pin_configure(fw_dev_t *dev, int number, int mode)
{
    fw_host_t *host = FW_CONTAINER_OF(dev, fw_host_t, pin.dev);
    (void)number;
    (void)mode;
    ++host->pin_configures;
    return FW_OK;
}

/* 引脚读：返回最后一次写入的电平。 */
static int fake_pin_read(fw_dev_t *dev, int number)
{
    fw_host_t *host = FW_CONTAINER_OF(dev, fw_host_t, pin.dev);
    (void)number;
    return host->pin_level ? 1 : 0;
}

/* 引脚写：记录电平并累计写次数。 */
static int fake_pin_write(fw_dev_t *dev, int number, int value)
{
    fw_host_t *host = FW_CONTAINER_OF(dev, fw_host_t, pin.dev);
    (void)number;
    host->pin_level = value != 0;
    ++host->pin_writes;
    return FW_OK;
}

static const fw_pin_ops_t fake_pin_ops = {
    fake_pin_configure,
    fake_pin_read,
    fake_pin_write
};

static const fw_drv_t fake_pin_drv = {
    FW_DEV_TYPE_PIN, &fake_pin_ops, NULL
};

/* 显示初始化：无硬件，直接成功。 */
static fw_status_t fake_display_init(void *ctx)
{
    (void)ctx;
    return FW_OK;
}

/*
 * 开始窗口：记录窗口参数并清空像素计数。
 *
 * 这样测试可以直接检查类层裁剪后的 x/y/width/height，
 * 以及一次 fill/bitmap 实际写了多少像素。
 */
static fw_status_t fake_display_begin(void *ctx, uint16_t x, uint16_t y,
                                      uint16_t width, uint16_t height,
                                      uint32_t timeout_ms)
{
    fw_host_t *host = (fw_host_t *)ctx;
    (void)timeout_ms;
    host->display_writing = true;
    host->display_x = x;
    host->display_y = y;
    host->display_width = width;
    host->display_height = height;
    host->display_pixel_count = 0U;
    ++host->display_begins;
    return FW_OK;
}

/*
 * 写像素：追加到 display_pixels。
 * 必须处于 begin..end 之间；容量不足返回 FW_EOVERFLOW。
 */
static fw_status_t fake_display_write(void *ctx, const uint16_t *pixels,
                                      size_t count, uint32_t timeout_ms)
{
    fw_host_t *host = (fw_host_t *)ctx;
    size_t room = sizeof(host->display_pixels) / sizeof(host->display_pixels[0]);
    (void)timeout_ms;
    if (!host->display_writing) return FW_EINVAL;
    if (count > room - host->display_pixel_count) return FW_EOVERFLOW;
    memcpy(&host->display_pixels[host->display_pixel_count], pixels,
           count * sizeof(uint16_t));
    host->display_pixel_count += count;
    return FW_OK;
}

/* 结束窗口：清除 writing 并计数。 */
static fw_status_t fake_display_end(void *ctx)
{
    fw_host_t *host = (fw_host_t *)ctx;
    host->display_writing = false;
    ++host->display_ends;
    return FW_OK;
}

/* 背光：记录逻辑状态，不区分有效电平。 */
static fw_status_t fake_display_backlight(void *ctx, bool on)
{
    fw_host_t *host = (fw_host_t *)ctx;
    host->backlight = on;
    ++host->backlight_changes;
    return FW_OK;
}

static const fw_display_ops_t fake_display_ops = {
    fake_display_init,
    fake_display_begin,
    fake_display_write,
    fake_display_end,
    fake_display_backlight
};

/*
 * 复位测试环境。
 *
 * 清零所有记录、复位设备注册表，并把 write_limit 设为最大容量。
 * 不安装延时钩子，交给 host_init 统一设置。
 */
void host_reset(fw_host_t *host)
{
    memset(host, 0, sizeof(*host));
    host->write_limit = HOST_SERIAL_CAPACITY;
    host->display_writing = false;
    fw_dev_registry_reset();
}

/* 初始化环境并安装假延时钩子。 */
void host_init(fw_host_t *host)
{
    host_reset(host);
    fw_clock_set_delay_fn(host_delay);
}

/* 查询 RX ring 中未读字节数。 */
static size_t rx_available(const fw_host_t *host)
{
    return host->serial.rd_fifo.count;
}

size_t host_serial_rx_available(const fw_host_t *host)
{
    return rx_available(host);
}

/*
 * 向串口注入接收数据。
 *
 * 直接写 rd_fifo，模拟“USART ISR 已经把字节放进 ring”的状态；
 * 不经过 read_byte/ISR 路径，便于精确控制测试输入。
 */
size_t host_serial_feed(fw_host_t *host, const uint8_t *data, size_t length)
{
    return fw_ring_write(&host->serial.rd_fifo, data, length);
}

/*
 * 调整 TX ring 容量。
 *
 * 重新初始化 wr_fifo 会丢弃现有数据；测试在发送前调用，
 * 用来模拟“ring 很小，一次只能接收几个字节”的短写场景。
 */
void host_serial_set_write_limit(fw_host_t *host, size_t limit)
{
    size_t capacity = limit == 0U ? 1U : limit;
    if (capacity > sizeof(host->wr_storage)) capacity = sizeof(host->wr_storage);
    host->write_limit = capacity;
    (void)fw_ring_init(&host->serial.wr_fifo, host->wr_storage, capacity);
}

/*
 * 注册三个假设备。
 *
 * 顺序与真实板级一致：serial、pin、lcd；
 * 这里不模拟依赖关系，注册顺序只影响设备表下标。
 * 返回值被忽略，因为测试环境已知这些名字不会冲突。
 */
void host_register_devices(fw_host_t *host)
{
    fw_display_info_t info = { HOST_DISPLAY_WIDTH, HOST_DISPLAY_HEIGHT, 16U };
    (void)fw_serial_register(&host->serial, "serial1", &fake_serial_drv,
                             host->rx_storage, sizeof(host->rx_storage),
                             host->wr_storage, host->write_limit, NULL);
    (void)fw_pin_register(&host->pin, "pin", &fake_pin_drv);
    (void)fw_display_register(&host->display, "lcd", &fake_display_ops,
                              host, &info);
}

/* 构造假时钟，ctx 指向 host，使 now_ms 可被测试推进。 */
fw_clock_t host_clock(fw_host_t *host)
{
    fw_clock_t clock = { host_now, host };
    return clock;
}

/* 推进假时钟；只影响依赖 fw_clock_t 的逻辑，不影响真实时间。 */
void host_advance(fw_host_t *host, uint32_t ms)
{
    host->now_ms += ms;
}
