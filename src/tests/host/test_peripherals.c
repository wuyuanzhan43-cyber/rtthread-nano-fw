/*
 * test_peripherals.c - DMA/EXTI 相关框架语义的 Host 测试。
 *
 * 层级：src/tests/host
 * 职责：
 *   1. 验证 ring peek/discard 的“不消费/只消费”语义；
 *   2. 验证 serial 类给 DMA 驱动使用的 rx_push/tx_peek/tx_consume；
 *   3. 验证 pin 类 attach/detach/enable 与 ISR 回调分派；
 *   4. 验证 SPI 类 async/wait/abort 的 busy 状态机。
 *
 * 这些测试不模拟 STM32 DMA/EXTI/SPI 寄存器，只覆盖可以脱离硬件验证的
 * “策略与所有权”部分；真实时序仍由 HIL/上板验证。
 */
#include "test_common.h"
#include "host_devices.h"
#include "fw/dev.h"
#include "fw/pin.h"
#include "fw/ring.h"
#include "fw/serial.h"
#include "fw/spi.h"

/* pin IRQ 回调的静态记录；回调在测试里仍按 ISR 上下文约束编写。 */
static unsigned s_irq_count;
static int s_irq_number;
static int s_irq_mode;

/* 只实现一半 async 扩展的驱动；注册必须被拒绝。 */
static int s_partial_configure(fw_dev_t *dev, const fw_spi_config_t *config)
{
    (void)dev;
    (void)config;
    return FW_OK;
}

static int s_partial_transfer(fw_dev_t *dev, const uint8_t *tx, uint8_t *rx,
                              size_t length, uint32_t timeout_ms)
{
    (void)dev;
    (void)tx;
    (void)rx;
    (void)timeout_ms;
    return (int)length;
}

static int s_partial_async(fw_dev_t *dev, const uint8_t *tx, uint8_t *rx,
                           size_t length)
{
    (void)dev;
    (void)tx;
    (void)rx;
    return (int)length;
}

static const fw_spi_ops_t s_partial_ops = {
    s_partial_configure,
    s_partial_transfer,
    s_partial_async,
    NULL,
    NULL
};

static const fw_drv_t s_partial_drv = {
    FW_DEV_TYPE_SPI, &s_partial_ops, NULL
};

static void test_pin_irq_cb(fw_dev_t *dev, int number, int mode, void *user)
{
    (void)dev;
    (void)user;
    ++s_irq_count;
    s_irq_number = number;
    s_irq_mode = mode;
}

/*
 * ring 的 peek/discard 语义。
 *
 * DMA TX 依赖这两个动作：
 *   - peek 把最早的一块复制到 staging，但不移动 tail；
 *   - consume 在 DMA 完成中断里只移动 tail/count。
 * 如果 peek 意外消费了数据，DMA 启动前就会丢块；如果 discard 写坏
 * head/count，后续生产者会覆盖未读数据。
 */
static void test_ring_peek_discard(void)
{
    uint8_t storage[4];
    uint8_t data[4];
    fw_ring_t ring;

    CHECK(fw_ring_init(&ring, storage, sizeof(storage)) == FW_OK);
    CHECK(fw_ring_write(&ring, (const uint8_t *)"abcd", 4U) == 4U);

    CHECK(fw_ring_peek(&ring, data, 2U) == 2U);
    CHECK(memcmp(data, "ab", 2U) == 0);
    CHECK(ring.count == 4U); /* peek 不能改变有效数据量 */

    CHECK(fw_ring_discard(&ring, 2U) == 2U);
    CHECK(ring.count == 2U);
    CHECK(fw_ring_read(&ring, data, sizeof(data)) == 2U);
    CHECK(memcmp(data, "cd", 2U) == 0);
}

/*
 * serial DMA 辅助函数。
 *
 * 直接操作类的 ring，验证：
 *   - rx_push 把块写入 rd_fifo，任务侧 read 能读到；
 *   - tx_peek 不消费，tx_consume 才推进 tail；
 *   - rx_push 短写时设置 FW_EOVERFLOW。
 */
static void test_serial_dma_helpers(void)
{
    fw_host_t host;
    uint8_t block[8];
    uint8_t readback[8];
    int result;

    host_init(&host);
    host_register_devices(&host);

    /* RX：DMA 完成块进入 ring。 */
    CHECK(fw_serial_rx_push(&host.serial, (const uint8_t *)"abc", 3U) == 3U);
    result = fw_dev_read(&host.serial.dev, 0, readback, sizeof(readback), 0U);
    CHECK(result == 3 && memcmp(readback, "abc", 3U) == 0);

    /* TX：peek 只复制，不消费。 */
    CHECK(fw_ring_write(&host.serial.wr_fifo, (const uint8_t *)"hello", 5U) == 5U);
    CHECK(fw_serial_tx_peek(&host.serial, block, 2U) == 2U);
    CHECK(memcmp(block, "he", 2U) == 0);
    CHECK(host.serial.wr_fifo.count == 5U);

    /* 完成中断确认后只消费 2 字节。 */
    CHECK(fw_serial_tx_consume(&host.serial, 2U) == 2U);
    CHECK(host.serial.wr_fifo.count == 3U);
    CHECK(fw_ring_read(&host.serial.wr_fifo, readback, sizeof(readback)) == 3U);
    CHECK(memcmp(readback, "llo", 3U) == 0);

    /* RX ring 满时，push 短写并置错误。 */
    CHECK(fw_serial_rx_push(&host.serial, block, 0U) == 0U);
    {
        uint8_t big[HOST_SERIAL_CAPACITY];
        memset(big, 0x5A, sizeof(big));
        CHECK(fw_serial_rx_push(&host.serial, big, sizeof(big)) == sizeof(big));
        CHECK(fw_serial_rx_push(&host.serial, block, 1U) == 0U);
        CHECK(host.serial.error == FW_EOVERFLOW);
    }
}

/*
 * pin IRQ 绑定、注入、解绑。
 *
 * 注入走 fw_dev_isr -> pin 类 isr -> 回调表，和真实驱动的 EXTI ISR
 * 分派路径一致；fake driver 只记录 attach/detach/enable 调用。
 */
static void test_pin_irq(void)
{
    fw_host_t host;
    fw_dev_t *pin;

    host_init(&host);
    host_register_devices(&host);
    pin = fw_dev_find("pin");
    CHECK(pin != NULL);

    s_irq_count = 0U;
    s_irq_number = -1;
    s_irq_mode = 0;

    CHECK(fw_pin_attach_irq(pin, HOST_LED_PIN, FW_PIN_IRQ_BOTH,
                            test_pin_irq_cb, NULL) == FW_OK);
    CHECK(host.pin_irq_attaches == 1U);
    CHECK(host_pin_inject_irq(&host, HOST_LED_PIN, FW_PIN_IRQ_BOTH) == FW_OK);
    CHECK(s_irq_count == 1U);
    CHECK(s_irq_number == HOST_LED_PIN);
    CHECK(s_irq_mode == FW_PIN_IRQ_BOTH);

    /* 重复 attach 同一引脚：更新回调/模式，不新增表项。 */
    CHECK(fw_pin_attach_irq(pin, HOST_LED_PIN, FW_PIN_IRQ_RISING,
                            test_pin_irq_cb, NULL) == FW_OK);
    CHECK(host.pin_irq_attaches == 2U);
    CHECK(host_pin_inject_irq(&host, HOST_LED_PIN, FW_PIN_IRQ_RISING) == FW_OK);
    CHECK(s_irq_count == 2U);
    CHECK(s_irq_mode == FW_PIN_IRQ_RISING);

    CHECK(fw_pin_irq_enable(pin, HOST_LED_PIN, false) == FW_OK);
    CHECK(host.pin_irq_enables == 1U);

    CHECK(fw_pin_detach_irq(pin, HOST_LED_PIN) == FW_OK);
    CHECK(host.pin_irq_detaches == 1U);
    CHECK(host_pin_inject_irq(&host, HOST_LED_PIN, FW_PIN_IRQ_RISING)
          == FW_ENOTREADY);

    /* 非法模式必须被类层拒绝，不能配到硬件。 */
    CHECK(fw_pin_attach_irq(pin, HOST_LED_PIN, 0,
                            test_pin_irq_cb, NULL) == FW_ENOTSUP);
}

/*
 * SPI 类 async/wait/abort 状态机。
 *
 * fake SPI 在 wait 成功时搬运数据，因此可以精确制造：
 *   - 启动后 busy，阻止第二次 async/同步 transfer/重配；
 *   - timeout==0 未完成返回 EAGAIN 且 busy 保持；
 *   - 正常完成/错误结果清 busy；
 *   - wait 超时由类层 abort，abort 失败时 busy 保持。
 */
static void test_spi_async(void)
{
    fw_host_t host;
    fw_spi_t *spi;
    uint8_t tx[4] = {1U, 2U, 3U, 4U};
    uint8_t rx[4] = {0};
    fw_spi_config_t config = { 6000000U, 1, 0 };

    host_init(&host);
    host_register_devices(&host);
    spi = fw_spi_find("spi1");
    CHECK(spi != NULL);
    if (spi == NULL) return;

    /* 非 busy 时重配成功，并记录软件副本。 */
    CHECK(fw_dev_ioctl(&spi->dev, 0, FW_CTL_SPI_SET_CONFIG, &config) ==
          (int)sizeof(config));
    CHECK(host.spi_config.mode == 1 && host.spi_config.bit_order == 0);

    /* 同步传输仍然可用。 */
    CHECK(fw_spi_transfer(&spi->dev, tx, rx, sizeof(tx), 10U) ==
          (int)sizeof(tx));
    CHECK(host.spi_tx_length == 4U);
    CHECK(memcmp(host.spi_tx, tx, 4U) == 0);
    CHECK(rx[0] == 0x40U && rx[3] == 0x43U);

    /* 异步启动：返回长度、busy 置位，并阻止重入。 */
    host.spi_tx_length = 0U;
    CHECK(fw_spi_transfer_async(&spi->dev, tx, NULL, sizeof(tx)) == 4);
    CHECK(spi->busy);
    CHECK(fw_spi_transfer_async(&spi->dev, tx, NULL, sizeof(tx)) == FW_EBUSY);
    CHECK(fw_spi_transfer(&spi->dev, tx, NULL, sizeof(tx), 10U) == FW_EBUSY);
    CHECK(fw_dev_ioctl(&spi->dev, 0, FW_CTL_SPI_SET_CONFIG, &config) ==
          FW_EBUSY);

    /* timeout==0 轮询：未完成 -> EAGAIN，busy 必须保持。 */
    host.spi_wait_result = FW_EAGAIN;
    CHECK(fw_spi_transfer_wait(&spi->dev, 0U) == FW_EAGAIN);
    CHECK(spi->busy);

    /* 完成：数据在 wait 成功时才进入 spi_tx。 */
    host.spi_wait_result = FW_OK;
    CHECK(fw_spi_transfer_wait(&spi->dev, 10U) == FW_OK);
    CHECK(!spi->busy);
    CHECK(host.spi_tx_length == 4U && memcmp(host.spi_tx, tx, 4U) == 0);

    /* 硬件错误：wait 返回 EIO 并清 busy。 */
    CHECK(fw_spi_transfer_async(&spi->dev, tx, NULL, 4U) == 4);
    host.spi_wait_result = FW_EIO;
    CHECK(fw_spi_transfer_wait(&spi->dev, 10U) == FW_EIO);
    CHECK(!spi->busy);

    /* 等待超时：类层调用 abort，成功则清 busy。 */
    host.spi_wait_result = FW_OK;
    CHECK(fw_spi_transfer_async(&spi->dev, tx, rx, 4U) == 4);
    host.spi_wait_result = FW_ETIMEOUT;
    CHECK(fw_spi_transfer_wait(&spi->dev, 10U) == FW_ETIMEOUT);
    CHECK(host.spi_abort_calls == 1U);
    CHECK(!spi->busy);

    /* abort 失败：返回 EIO 且 busy 保持，允许显式重试。 */
    host.spi_wait_result = FW_OK;
    CHECK(fw_spi_transfer_async(&spi->dev, tx, NULL, 4U) == 4);
    host.spi_wait_result = FW_ETIMEOUT;
    host.spi_abort_result = FW_EIO;
    CHECK(fw_spi_transfer_wait(&spi->dev, 10U) == FW_EIO);
    CHECK(spi->busy);
    host.spi_abort_result = FW_OK;
    CHECK(fw_spi_transfer_abort(&spi->dev) == FW_OK);
    CHECK(!spi->busy);
    CHECK(fw_spi_transfer_abort(&spi->dev) == FW_OK); /* 幂等 */

    /* 驱动报告 EBUSY（清理未完成）：类层返回 EIO 但保留 busy。 */
    host.spi_wait_result = FW_OK;
    CHECK(fw_spi_transfer_async(&spi->dev, tx, NULL, 4U) == 4);
    host.spi_wait_result = FW_EBUSY;
    CHECK(fw_spi_transfer_wait(&spi->dev, 10U) == FW_EIO);
    CHECK(spi->busy);
    CHECK(fw_spi_transfer_abort(&spi->dev) == FW_OK);
    CHECK(!spi->busy);

    /* 注册不变量：async/wait/abort 必须全有或全无。 */
    {
        fw_spi_t partial;
        fw_spi_config_t partial_config = { 1000000U, 0, 1 };
        CHECK(fw_spi_register(&partial, "spi_partial", &s_partial_drv,
                              &partial_config) == FW_EINVAL);
        CHECK(fw_dev_find("spi_partial") == NULL);
    }

    /* 参数检查。 */
    CHECK(fw_spi_transfer_async(&spi->dev, NULL, NULL, 4U) == FW_EINVAL);
    CHECK(fw_spi_transfer_async(&spi->dev, tx, NULL,
                                FW_SPI_MAX_TRANSFER + 1U) == FW_EINVAL);
    CHECK(fw_spi_transfer_wait(&spi->dev, 1U) == FW_EINVAL);
}

void test_peripherals_run(void)
{
    test_ring_peek_discard();
    test_serial_dma_helpers();
    test_pin_irq();
    test_spi_async();
}
