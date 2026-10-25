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

/* 假串口没有在途 DMA，flush 直接成功；ring 清空由类层完成。 */
static int fake_serial_flush(fw_dev_t *dev, bool rx)
{
    (void)dev;
    (void)rx;
    return FW_OK;
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
    fake_serial_unlock,
    fake_serial_flush
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

/*
 * 假 EXTI 配置：只记录调用，不模拟硬件位。
 * 真正的中断注入由 host_pin_inject_irq 调用 fw_dev_isr 完成，
 * 这样测试覆盖 pin 类回调表与驱动 ISR 分派两层语义。
 */
static int fake_pin_attach_irq(fw_dev_t *dev, int number, int mode)
{
    fw_host_t *host = FW_CONTAINER_OF(dev, fw_host_t, pin.dev);
    (void)number;
    (void)mode;
    ++host->pin_irq_attaches;
    return FW_OK;
}

static int fake_pin_detach_irq(fw_dev_t *dev, int number)
{
    fw_host_t *host = FW_CONTAINER_OF(dev, fw_host_t, pin.dev);
    (void)number;
    ++host->pin_irq_detaches;
    return FW_OK;
}

static int fake_pin_enable_irq(fw_dev_t *dev, int number, bool enable)
{
    fw_host_t *host = FW_CONTAINER_OF(dev, fw_host_t, pin.dev);
    (void)number;
    (void)enable;
    ++host->pin_irq_enables;
    return FW_OK;
}

static const fw_pin_ops_t fake_pin_ops = {
    fake_pin_configure,
    fake_pin_read,
    fake_pin_write,
    fake_pin_attach_irq,
    fake_pin_detach_irq,
    fake_pin_enable_irq
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

/* SPI 配置：只保存软件副本，硬件无动作。 */
static int fake_spi_configure(fw_dev_t *dev, const fw_spi_config_t *config)
{
    fw_host_t *host = FW_CONTAINER_OF(dev, fw_host_t, spi.dev);
    if (config == NULL) return FW_EINVAL;
    host->spi_config = *config;
    return FW_OK;
}

/*
 * 同步传输：把 TX 追加到 spi_tx，RX 填 0x40.. 模式。
 * 假设备不模拟时钟，收发在同一调用内“立即”完成。
 */
static int fake_spi_transfer(fw_dev_t *dev, const uint8_t *tx, uint8_t *rx,
                             size_t length, uint32_t timeout_ms)
{
    fw_host_t *host = FW_CONTAINER_OF(dev, fw_host_t, spi.dev);
    size_t i;
    (void)timeout_ms;
    if (length == 0U || (tx == NULL && rx == NULL)) return FW_EINVAL;
    if (tx != NULL) {
        if (host->spi_tx_length + length > sizeof(host->spi_tx))
            return FW_EOVERFLOW;
        memcpy(&host->spi_tx[host->spi_tx_length], tx, length);
        host->spi_tx_length += length;
    }
    if (rx != NULL) {
        for (i = 0U; i < length; ++i) rx[i] = (uint8_t)(0x40U + i);
    }
    return (int)length;
}

/*
 * 异步启动：只登记 pending，真正搬运在 wait 返回 FW_OK 时完成。
 * 这样测试可以精确制造 EAGAIN、超时和 abort 窗口。
 */
static int fake_spi_transfer_async(fw_dev_t *dev, const uint8_t *tx,
                                   uint8_t *rx, size_t length)
{
    fw_host_t *host = FW_CONTAINER_OF(dev, fw_host_t, spi.dev);
    if (length == 0U || (tx == NULL && rx == NULL)) return FW_EINVAL;
    if (host->spi_pending) return FW_EBUSY;
    host->spi_pending = true;
    host->spi_pending_length = length;
    host->spi_pending_tx = tx;
    host->spi_pending_rx = rx;
    return (int)length;
}

/* 等待：EAGAIN/ETIMEOUT 保留 pending，其余结果完成并清 pending。 */
static int fake_spi_transfer_wait(fw_dev_t *dev, uint32_t timeout_ms)
{
    fw_host_t *host = FW_CONTAINER_OF(dev, fw_host_t, spi.dev);
    size_t i;
    (void)timeout_ms;
    if (!host->spi_pending) return FW_EINVAL;
    if (host->spi_wait_result == FW_EAGAIN ||
        host->spi_wait_result == FW_ETIMEOUT)
        return host->spi_wait_result;

    if (host->spi_wait_result == FW_OK) {
        if (host->spi_pending_tx != NULL) {
            if (host->spi_tx_length + host->spi_pending_length >
                sizeof(host->spi_tx))
                return FW_EOVERFLOW;
            memcpy(&host->spi_tx[host->spi_tx_length], host->spi_pending_tx,
                   host->spi_pending_length);
            host->spi_tx_length += host->spi_pending_length;
        }
        if (host->spi_pending_rx != NULL) {
            for (i = 0U; i < host->spi_pending_length; ++i)
                host->spi_pending_rx[i] = (uint8_t)(0x40U + i);
        }
    }

    host->spi_pending = false;
    host->spi_pending_tx = NULL;
    host->spi_pending_rx = NULL;
    return host->spi_wait_result;
}

/* 取消：记录调用次数；abort_result 由测试注入，失败时保留 pending。 */
static int fake_spi_transfer_abort(fw_dev_t *dev)
{
    fw_host_t *host = FW_CONTAINER_OF(dev, fw_host_t, spi.dev);
    ++host->spi_abort_calls;
    if (host->spi_abort_result != FW_OK) return host->spi_abort_result;
    host->spi_pending = false;
    host->spi_pending_tx = NULL;
    host->spi_pending_rx = NULL;
    return FW_OK;
}

static const fw_spi_ops_t fake_spi_ops = {
    fake_spi_configure,
    fake_spi_transfer,
    fake_spi_transfer_async,
    fake_spi_transfer_wait,
    fake_spi_transfer_abort
};

static const fw_drv_t fake_spi_drv = {
    FW_DEV_TYPE_SPI, &fake_spi_ops, NULL
};

/* ----------------------------- fake I2C ----------------------------- */

static int fake_i2c_configure(fw_dev_t *dev, const fw_i2c_config_t *config)
{
    fw_host_t *host = FW_CONTAINER_OF(dev, fw_host_t, i2c.dev);
    if (config == NULL) return FW_EINVAL;
    host->i2c_config = *config;
    return FW_OK;
}

/* 原始事务：记录地址和 TX，RX 填 0x80.. 模式。 */
static int fake_i2c_transfer(fw_dev_t *dev, uint16_t addr7,
                             const uint8_t *tx, size_t tx_len,
                             uint8_t *rx, size_t rx_len, uint32_t timeout_ms)
{
    fw_host_t *host = FW_CONTAINER_OF(dev, fw_host_t, i2c.dev);
    size_t i;
    (void)timeout_ms;
    ++host->i2c_transfers;
    host->i2c_last_addr = addr7;
    if (host->i2c_result != FW_OK) return host->i2c_result;
    if (tx_len != 0U) {
        if (host->i2c_tx_length + tx_len > sizeof(host->i2c_tx))
            return FW_EOVERFLOW;
        memcpy(&host->i2c_tx[host->i2c_tx_length], tx, tx_len);
        host->i2c_tx_length += tx_len;
    }
    if (rx_len != 0U) {
        for (i = 0U; i < rx_len; ++i) rx[i] = (uint8_t)(0x80U + i);
    }
    return (int)(tx_len + rx_len);
}

static int fake_i2c_mem_read(fw_dev_t *dev, uint16_t addr7, uint16_t mem_addr,
                             uint8_t mem_addr_size, uint8_t *rx,
                             size_t rx_len, uint32_t timeout_ms)
{
    fw_host_t *host = FW_CONTAINER_OF(dev, fw_host_t, i2c.dev);
    size_t i;
    (void)timeout_ms;
    ++host->i2c_mem_reads;
    host->i2c_last_addr = addr7;
    host->i2c_last_mem_addr = mem_addr;
    host->i2c_last_mem_size = mem_addr_size;
    if (host->i2c_result != FW_OK) return host->i2c_result;
    for (i = 0U; i < rx_len; ++i) rx[i] = (uint8_t)(0x40U + i);
    return (int)rx_len;
}

static int fake_i2c_mem_write(fw_dev_t *dev, uint16_t addr7,
                              uint16_t mem_addr, uint8_t mem_addr_size,
                              const uint8_t *tx, size_t tx_len,
                              uint32_t timeout_ms)
{
    fw_host_t *host = FW_CONTAINER_OF(dev, fw_host_t, i2c.dev);
    (void)timeout_ms;
    ++host->i2c_mem_writes;
    host->i2c_last_addr = addr7;
    host->i2c_last_mem_addr = mem_addr;
    host->i2c_last_mem_size = mem_addr_size;
    if (host->i2c_result != FW_OK) return host->i2c_result;
    if (host->i2c_tx_length + tx_len > sizeof(host->i2c_tx))
        return FW_EOVERFLOW;
    memcpy(&host->i2c_tx[host->i2c_tx_length], tx, tx_len);
    host->i2c_tx_length += tx_len;
    return (int)tx_len;
}

static int fake_i2c_probe(fw_dev_t *dev, uint16_t addr7, uint32_t timeout_ms)
{
    fw_host_t *host = FW_CONTAINER_OF(dev, fw_host_t, i2c.dev);
    (void)timeout_ms;
    ++host->i2c_probes;
    host->i2c_last_addr = addr7;
    return host->i2c_probe_result;
}

static const fw_i2c_ops_t fake_i2c_ops = {
    fake_i2c_configure,
    fake_i2c_transfer,
    fake_i2c_mem_read,
    fake_i2c_mem_write,
    fake_i2c_probe
};

static const fw_drv_t fake_i2c_drv = {
    FW_DEV_TYPE_I2C, &fake_i2c_ops, NULL
};

/* ----------------------------- fake ADC ----------------------------- */

static int fake_adc_configure(fw_dev_t *dev, const fw_adc_config_t *config)
{
    fw_host_t *host = FW_CONTAINER_OF(dev, fw_host_t, adc.dev);
    if (config == NULL) return FW_EINVAL;
    host->adc_config = *config;
    return FW_OK;
}

static int fake_adc_read(fw_dev_t *dev, int channel, uint16_t *raw,
                         uint32_t timeout_ms)
{
    fw_host_t *host = FW_CONTAINER_OF(dev, fw_host_t, adc.dev);
    (void)timeout_ms;
    ++host->adc_reads;
    if (host->adc_result != FW_OK) return host->adc_result;
    if (raw == NULL || channel < 0 || channel > 15) return FW_EINVAL;
    *raw = host->adc_samples[channel];
    return FW_OK;
}

static const fw_adc_ops_t fake_adc_ops = {
    fake_adc_configure,
    fake_adc_read
};

static const fw_drv_t fake_adc_drv = {
    FW_DEV_TYPE_ADC, &fake_adc_ops, NULL
};

/* ----------------------------- fake PWM ----------------------------- */

static int fake_pwm_configure(fw_dev_t *dev, const fw_pwm_config_t *config)
{
    fw_host_t *host = FW_CONTAINER_OF(dev, fw_host_t, pwm.dev);
    if (config == NULL) return FW_EINVAL;
    if (host->pwm_started) return FW_EBUSY;
    if (host->pwm_result != FW_OK) return host->pwm_result;
    host->pwm_config = *config;
    return FW_OK;
}

static int fake_pwm_set_duty(fw_dev_t *dev, int channel, uint32_t permille)
{
    fw_host_t *host = FW_CONTAINER_OF(dev, fw_host_t, pwm.dev);
    ++host->pwm_set_calls;
    if (host->pwm_result != FW_OK) return host->pwm_result;
    if (channel < 0 || channel >= FW_PWM_CHANNELS) return FW_EINVAL;
    host->pwm_duty[channel] = permille;
    return FW_OK;
}

static int fake_pwm_start(fw_dev_t *dev)
{
    fw_host_t *host = FW_CONTAINER_OF(dev, fw_host_t, pwm.dev);
    if (host->pwm_result != FW_OK) return host->pwm_result;
    host->pwm_started = true;
    return FW_OK;
}

static int fake_pwm_stop(fw_dev_t *dev)
{
    fw_host_t *host = FW_CONTAINER_OF(dev, fw_host_t, pwm.dev);
    if (host->pwm_result != FW_OK) return host->pwm_result;
    host->pwm_started = false;
    return FW_OK;
}

static const fw_pwm_ops_t fake_pwm_ops = {
    fake_pwm_configure,
    fake_pwm_set_duty,
    fake_pwm_start,
    fake_pwm_stop
};

static const fw_drv_t fake_pwm_drv = {
    FW_DEV_TYPE_PWM, &fake_pwm_ops, NULL
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
    host->spi_wait_result = FW_OK;
    host->spi_abort_result = FW_OK;
    host->i2c_result = FW_OK;
    host->i2c_probe_result = FW_OK;
    host->adc_result = FW_OK;
    host->pwm_result = FW_OK;
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
 * 注入一次 pin 边沿事件。
 *
 * 不直接调用应用回调，而是走 fw_dev_isr -> pin 类 isr -> 回调表，
 * 这样 Host 测试验证的是与真实驱动相同的分派路径。
 */
int host_pin_inject_irq(fw_host_t *host, int number, int mode)
{
    fw_dev_t *dev = fw_dev_find("pin");
    (void)mode;
    (void)host;
    if (dev == NULL) return FW_ENODEV;
    return fw_dev_isr(dev, FW_ISR_PIN_EDGE, &number);
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
 * 注册七个假设备。
 *
 * 顺序与真实板级一致：serial、pin、spi、i2c、adc、pwm、lcd；
 * 这里不模拟依赖关系，注册顺序只影响设备表下标。
 * 返回值被忽略，因为测试环境已知这些名字不会冲突。
 */
void host_register_devices(fw_host_t *host)
{
    fw_display_info_t info = { HOST_DISPLAY_WIDTH, HOST_DISPLAY_HEIGHT, 16U };
    fw_spi_config_t spi_config = { 12000000U, 0, 1 };
    fw_i2c_config_t i2c_config = { 100000U };
    fw_adc_config_t adc_config = { 3300U, 12U, 84U };
    fw_pwm_config_t pwm_config = { 1000U };
    (void)fw_serial_register(&host->serial, "serial1", &fake_serial_drv,
                             host->rx_storage, sizeof(host->rx_storage),
                             host->wr_storage, host->write_limit, NULL);
    (void)fw_pin_register(&host->pin, "pin", &fake_pin_drv);
    (void)fw_spi_register(&host->spi, "spi1", &fake_spi_drv, &spi_config);
    (void)fw_i2c_register(&host->i2c, "i2c1", &fake_i2c_drv, &i2c_config);
    (void)fw_adc_register(&host->adc, "adc1", &fake_adc_drv, &adc_config);
    (void)fw_pwm_register(&host->pwm, "pwm4", &fake_pwm_drv, &pwm_config);
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
