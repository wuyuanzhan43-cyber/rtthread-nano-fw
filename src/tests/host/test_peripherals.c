/*
 * test_peripherals.c - DMA/EXTI 相关框架语义的 Host 测试。
 *
 * 层级：src/tests/host
 * 职责：
 *   1. 验证 ring peek/discard 的“不消费/只消费”语义；
 *   2. 验证 serial 类给 DMA 驱动使用的 rx_push/tx_peek/tx_consume；
 *   3. 验证 pin 类 attach/detach/enable 与 ISR 回调分派。
 *
 * 这些测试不模拟 STM32 DMA/EXTI 寄存器，只覆盖可以脱离硬件验证的
 * “策略与所有权”部分；真实时序仍由 HIL/上板验证。
 */
#include "test_common.h"
#include "host_devices.h"
#include "fw/dev.h"
#include "fw/pin.h"
#include "fw/ring.h"
#include "fw/serial.h"

/* pin IRQ 回调的静态记录；回调在测试里仍按 ISR 上下文约束编写。 */
static unsigned s_irq_count;
static int s_irq_number;
static int s_irq_mode;

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

void test_peripherals_run(void)
{
    test_ring_peek_discard();
    test_serial_dma_helpers();
    test_pin_irq();
}
