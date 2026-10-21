/*
 * test_framework.c - framework 层 Host 单元测试。
 *
 * 层级：src/tests/host
 * 职责：
 *   1. 验证 ring、A5 5A 协议、设备注册表；
 *   2. 通过假设备验证 serial/pin/display 类；
 *   3. 不包含任何应用层业务命令或资源。
 *
 * 上下文：单线程。
 * 依赖：test_common.h、fw/dev.h、fw/ring.h、fw/serial.h、fw/pin.h、
 *       fw/display.h、fw/protocol.h、fw/status.h。
 */
#include "test_common.h"
#include "fw/dev.h"
#include "fw/ring.h"
#include "fw/serial.h"
#include "fw/pin.h"
#include "fw/display.h"
#include "fw/status.h"

/* 协议测试只关心帧格式，使用一个与业务无关的命令字。 */
#define TEST_PROTOCOL_COMMAND 0x02U

/*
 * ring 单元测试。
 *
 * 覆盖：
 *   - 写满后短写返回 0；
 *   - 读取部分数据后索引前移；
 *   - 环绕写入 "ef" 后能按顺序读出 "def"。
 */
static void test_ring(void)
{
    uint8_t storage[4];
    uint8_t data[4];
    size_t count;
    fw_ring_t ring;

    CHECK(fw_ring_init(&ring, storage, sizeof(storage)) == FW_OK);
    CHECK(fw_ring_write(&ring, (const uint8_t *)"abcd", 4U) == 4U);
    CHECK(fw_ring_write(&ring, (const uint8_t *)"e", 1U) == 0U);

    count = fw_ring_read(&ring, data, 3U);
    CHECK(count == 3U && memcmp(data, "abc", 3U) == 0);

    /* ring 现在剩 1 字节（d）+ 3 个空位，写 2 字节会环绕。 */
    CHECK(fw_ring_write(&ring, (const uint8_t *)"ef", 2U) == 2U);
    count = fw_ring_read(&ring, data, sizeof(data));
    CHECK(count == 3U && memcmp(data, "def", 3U) == 0);
}

/*
 * 协议单元测试。
 *
 * 固定一帧 command=0x02、payload=1：
 *   A5 5A 01 01 02 01 CRC_LO CRC_HI
 * 并断言编码结果逐字节一致，再逐字节喂给 parser 还原字段。
 */
static void test_protocol(void)
{
    const uint8_t expected[] = {0xA5U, 0x5AU, 0x01U, 0x01U, 0x02U,
                                0x01U, 0x07U, 0xB3U};
    fw_message_t message = {0};
    fw_message_t parsed;
    fw_parser_t parser;
    uint8_t frame[FW_PROTOCOL_MAX_FRAME];
    size_t length = 0U;
    size_t i;
    bool got = false;

    message.sequence = 1U;
    message.command = TEST_PROTOCOL_COMMAND;
    message.length = 1U;
    message.payload[0] = 1U;

    CHECK(fw_protocol_encode(&message, frame, sizeof(frame), &length) == FW_OK);
    CHECK(length == sizeof(expected));
    CHECK(memcmp(frame, expected, sizeof(expected)) == 0);

    fw_parser_init(&parser);
    for (i = 0U; i < length; ++i) {
        if (fw_parser_push(&parser, frame[i], &parsed)) {
            got = true;
            break;
        }
    }
    CHECK(got);
    CHECK(parsed.sequence == 1U && parsed.command == TEST_PROTOCOL_COMMAND);
}

/*
 * 设备注册表测试。
 *
 * 覆盖：
 *   - 注册 3 个假设备后 count==3；
 *   - "serial1" 与 "/dev/pin" 两种路径写法都能找到；
 *   - 类型化查找会拒绝类型不匹配；
 *   - open/close 的引用计数；
 *   - 重复注册同名设备返回 FW_EEXIST；
 *   - 状态码字符串稳定。
 */
static void test_device_registry(void)
{
    fw_host_t host;
    fw_dev_t extra;
    fw_drv_t dummy = { FW_DEV_TYPE_PIN, NULL, NULL };
    fw_dev_t *found;

    host_init(&host);
    host_register_devices(&host);

    CHECK(fw_dev_count() == 3U);
    found = fw_dev_find("serial1");
    CHECK(found != NULL && found->type == FW_DEV_TYPE_SERIAL);
    CHECK(fw_dev_find("/dev/pin") != NULL);
    CHECK(fw_dev_find("/dev/lcd") != NULL);
    CHECK(fw_dev_find("missing") == NULL);
    CHECK(fw_serial_find("serial1") != NULL);
    CHECK(fw_serial_find("pin") == NULL);

    /* 第一次 open 触发类 open；close 后计数归零。 */
    CHECK(fw_dev_open(found, FW_OFLAG_RDWR) == FW_OK);
    CHECK(found->ref_count == 1U);
    CHECK(fw_dev_close(found) == FW_OK);
    CHECK(found->ref_count == 0U);

    /* 同名设备必须被拒绝，但可以注册一个新名字。 */
    CHECK(fw_dev_register(&extra, "extra", FW_DEV_TYPE_PIN, &dummy) == FW_OK);
    CHECK(fw_dev_register(&extra, "extra", FW_DEV_TYPE_PIN, &dummy) == FW_EEXIST);
    {
        fw_dev_t conflict;
        CHECK(fw_dev_register(&conflict, "pin", FW_DEV_TYPE_PIN, &dummy) == FW_EEXIST);
    }
    CHECK(fw_dev_count() == 4U);

    CHECK(strcmp(fw_status_name(FW_EBUSY), "EBUSY") == 0);
}

/*
 * serial 与 pin 类测试。
 *
 * 串口部分：
 *   - 注入 "hi"，read 返回 2 且内容正确；
 *   - 再次 read 返回 0（非阻塞无数据）；
 *   - wait_rx 在无数据且 timeout=0 时返回 EAGAIN；
 *   - 把 TX ring 限制为 2 字节，write("abc") 返回短写 2；
 *   - 恢复容量后写剩余 "c"，最终发送记录为 "abc"。
 *
 * pin 部分：
 *   - 写 1/0 后读回相同电平。
 */
static void test_serial_pin(void)
{
    fw_host_t host;
    uint8_t data[8];
    int result;

    host_init(&host);
    host_register_devices(&host);

    CHECK(host_serial_feed(&host, (const uint8_t *)"hi", 2U) == 2U);
    result = fw_dev_read(&host.serial.dev, 0, data, sizeof(data), 0U);
    CHECK(result == 2 && memcmp(data, "hi", 2U) == 0);
    CHECK(fw_dev_read(&host.serial.dev, 0, data, sizeof(data), 0U) == 0);
    CHECK(fw_serial_wait_rx(&host.serial.dev, 0U) == FW_EAGAIN);

    /* 模拟 TX ring 只能容纳 2 字节：write 返回短写计数。 */
    host_serial_set_write_limit(&host, 2U);
    result = fw_dev_write(&host.serial.dev, 0, (const uint8_t *)"abc", 3U, 0U);
    CHECK(result == 2);
    CHECK(host.tx_length == 2U);

    host_serial_set_write_limit(&host, HOST_SERIAL_CAPACITY);
    result = fw_dev_write(&host.serial.dev, 0, (const uint8_t *)"c", 1U, 0U);
    CHECK(result == 1);
    CHECK(host.tx_length == 3U && memcmp(host.tx, "abc", 3U) == 0);

    CHECK(fw_pin_write(&host.pin.dev, HOST_LED_PIN, 1) == 1);
    CHECK(fw_pin_read(&host.pin.dev, HOST_LED_PIN) == 1);
    CHECK(fw_pin_write(&host.pin.dev, HOST_LED_PIN, 0) == 1);
    CHECK(fw_pin_read(&host.pin.dev, HOST_LED_PIN) == 0);
}

/*
 * display 类测试。
 *
 * 覆盖：
 *   - 矩形填充：窗口 8x4 -> 32 像素，颜色一致；
 *   - 边界裁剪：从 (126,158) 开始的 10x10 只剩 2x2=4 像素；
 *   - 位图：3x2 的 6 个像素按顺序原样写入；
 *   - 背光：逻辑状态被记录。
 */
static void test_display(void)
{
    fw_host_t host;
    uint16_t image[6] = {1U, 2U, 3U, 4U, 5U, 6U};

    host_init(&host);
    host_register_devices(&host);
    CHECK(fw_display_find("lcd") == &host.display);

    CHECK(fw_display_fill_rect(&host.display, 10U, 20U, 8U, 4U,
                               0x1234U, 10U) == FW_OK);
    CHECK(host.display_begins == 1U && host.display_ends == 1U);
    CHECK(host.display_x == 10U && host.display_y == 20U);
    CHECK(host.display_width == 8U && host.display_height == 4U);
    CHECK(host.display_pixel_count == 32U);
    CHECK(host.display_pixels[0] == 0x1234U);

    /* 越过右下边界，类层应裁剪到 2x2。 */
    host.display_pixel_count = 0U;
    CHECK(fw_display_fill_rect(&host.display, 126U, 158U, 10U, 10U,
                               0xABCDU, 10U) == FW_OK);
    CHECK(host.display_pixel_count == 4U);
    CHECK(host.display_width == 2U && host.display_height == 2U);

    /* 位图按行优先顺序写入，内容不应改变。 */
    host.display_pixel_count = 0U;
    CHECK(fw_display_draw_bitmap(&host.display, 0U, 0U, 3U, 2U,
                                 image, 10U) == FW_OK);
    CHECK(host.display_pixel_count == 6U);
    CHECK(memcmp(host.display_pixels, image, sizeof(image)) == 0);

    CHECK(fw_display_set_backlight(&host.display, true) == FW_OK);
    CHECK(host.backlight);
}

/* 运行 framework 全部测试。 */
void test_framework_run(void)
{
    test_ring();
    test_protocol();
    test_device_registry();
    test_serial_pin();
    test_display();
}
