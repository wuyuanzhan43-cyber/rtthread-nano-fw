/*
 * test_main.c - v2 Host 单元测试。
 *
 * 层级：src/tests/host
 * 职责：
 *   1. 在 PC 上验证 ring、协议、设备注册表、serial/pin/display 类和 demo 应用；
 *   2. 不需要 RTOS/HAL，用 host_devices.c 的假设备替代底层；
 *   3. 作为 CTest 的第一个测试，失败时返回非 0。
 *
 * 上下文：单线程。
 * 依赖：host_devices.h、fw/dev.h、fw/ring.h、fw/protocol.h、
 *       fw/demo_app.h、fw/status.h。
 *
 * 测试策略：
 *   - 单元级：ring 的环绕/容量、协议的编码字节和解析；
 *   - 集成级：通过设备核心注册假设备，走真实的 open/read/write/ioctl；
 *   - 应用级：把协议帧喂给 demo_app，检查回复帧、LED、统计和背压。
 */
#include <stdio.h>
#include <string.h>
#include "host_devices.h"
#include "fw/dev.h"
#include "fw/ring.h"
#include "fw/protocol.h"
#include "fw/demo_app.h"
#include "fw/status.h"

/* 失败计数；main 最后据此决定退出码。 */
static int failures;

/*
 * 断言宏。
 *
 * 失败时打印文件、行号和表达式本身，但不中断测试，
 * 这样可以一次性看到所有失败项。
 */
#define CHECK(condition)                                                      \
    do {                                                                      \
        if (!(condition)) {                                                   \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);       \
            ++failures;                                                       \
        }                                                                     \
    } while (0)

/*
 * 编码并注入一帧请求。
 *
 * 用真实的 fw_protocol_encode 生成字节，再写入假串口的 RX ring，
 * 这样测试同时覆盖编码器和 parser 的输入路径。
 */
static void feed_frame(fw_host_t *host, uint8_t sequence, uint8_t command,
                       const uint8_t *payload, uint8_t length)
{
    fw_message_t message = {0};
    uint8_t frame[FW_PROTOCOL_MAX_FRAME];
    size_t frame_length = 0U;

    message.sequence = sequence;
    message.command = command;
    message.length = length;
    if (length != 0U) memcpy(message.payload, payload, length);

    CHECK(fw_protocol_encode(&message, frame, sizeof(frame), &frame_length) == FW_OK);
    CHECK(host_serial_feed(host, frame, frame_length) == frame_length);
}

/*
 * 从假串口发送记录中解析出第一帧并断言字段。
 *
 * 不假设 host->tx 中只有一帧：从第一个字节开始喂 parser，
 * 遇到完整帧就停止。调用者通常会在每次断言后清空 tx_length。
 */
static void expect_frame(fw_host_t *host, uint8_t sequence, uint8_t command,
                         const uint8_t *payload, uint8_t length)
{
    fw_parser_t parser;
    fw_message_t message;
    bool found = false;
    size_t i;

    fw_parser_init(&parser);
    for (i = 0U; i < host->tx_length; ++i) {
        if (fw_parser_push(&parser, host->tx[i], &message)) {
            found = true;
            break;
        }
    }
    CHECK(found);
    if (!found) return;

    CHECK(message.sequence == sequence);
    CHECK(message.command == command);
    CHECK(message.length == length);
    if (message.length == length && length != 0U) {
        CHECK(memcmp(message.payload, payload, length) == 0);
    }
}

/*
 * ring 单元测试。
 *
 * 覆盖：
 *   - 写满后短写返回 0；
 *   - 读取部分数据后索引前移；
 *   - 环绕写入 "ef" 后能按顺序读出 "def"。
 *
 * 这是串口 RX/TX 正确性的基础。
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
 * 固定一帧 LED_SET(seq=1, payload=1)：
 *   A5 5A 01 01 02 01 CRC_LO CRC_HI
 * 并断言编码结果逐字节一致。
 *
 * 然后逐字节喂给 parser，确认能还原 sequence/command。
 * 这同时锁定了 CRC 算法和帧布局，防止后续改动破坏线协议兼容性。
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
    message.command = FW_CMD_LED_SET;
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
    CHECK(parsed.sequence == 1U && parsed.command == FW_CMD_LED_SET);
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

/*
 * 初始化 demo 测试环境。
 *
 * 注册假设备并把 serial/pin/LED/clock 填入配置；
 * 设置与 main.c 相同的限制参数（250/20/64）。
 */
static void demo_setup(fw_host_t *host, fw_demo_app_t *app,
                       fw_demo_config_t *config)
{
    host_init(host);
    host_register_devices(host);
    config->serial = &host->serial.dev;
    config->pin = &host->pin.dev;
    config->led_pin = HOST_LED_PIN;
    config->led_active_high = true;
    config->clock = host_clock(host);
    CHECK(fw_demo_init(app, config) == FW_OK);
    CHECK(fw_demo_set_limits(app, 250U, 20U, 64U) == FW_OK);
}

/*
 * demo 基本命令测试。
 *
 * 依次发送 PING、LED_SET、STATUS、未知命令、STATS，
 * 检查回复帧的 sequence/command/payload、LED 电平和统计值。
 *
 * 每次检查后清空 host.tx_length，避免多帧混在一起。
 */
static void test_demo_basic(void)
{
    fw_host_t host;
    fw_demo_app_t app;
    fw_demo_config_t config;
    const uint8_t ping_reply[] = {0U, 'P', 'O', 'N', 'G'};
    const uint8_t led_on[] = {1U};
    const uint8_t led_reply[] = {0U};
    const uint8_t status_reply[] = {0U, 1U, 1U};
    const uint8_t unknown_reply[] = {2U};

    demo_setup(&host, &app, &config);

    feed_frame(&host, 1U, FW_CMD_PING, NULL, 0U);
    CHECK(fw_demo_poll(&app) == FW_OK);
    expect_frame(&host, 1U, (uint8_t)(FW_CMD_PING | 0x80U), ping_reply, 5U);
    host.tx_length = 0U;

    feed_frame(&host, 2U, FW_CMD_LED_SET, led_on, 1U);
    CHECK(fw_demo_poll(&app) == FW_OK);
    expect_frame(&host, 2U, (uint8_t)(FW_CMD_LED_SET | 0x80U), led_reply, 1U);
    CHECK(host.pin_level);
    host.tx_length = 0U;

    feed_frame(&host, 3U, FW_CMD_STATUS, NULL, 0U);
    CHECK(fw_demo_poll(&app) == FW_OK);
    expect_frame(&host, 3U, (uint8_t)(FW_CMD_STATUS | 0x80U), status_reply, 3U);
    host.tx_length = 0U;

    /* 0x7F 不在命令表中，回复状态码 2。 */
    feed_frame(&host, 4U, 0x7FU, NULL, 0U);
    CHECK(fw_demo_poll(&app) == FW_OK);
    expect_frame(&host, 4U, 0xFFU, unknown_reply, 1U);
    host.tx_length = 0U;

    /* STATS 回复 14 字节 payload；此时已处理 5 帧（含未知命令）。 */
    feed_frame(&host, 5U, FW_CMD_STATS, NULL, 0U);
    CHECK(fw_demo_poll(&app) == FW_OK);
    CHECK(host.tx_length == 21U);
    {
        fw_parser_t parser;
        fw_message_t stats;
        bool got = false;
        size_t i;
        fw_parser_init(&parser);
        for (i = 0U; i < host.tx_length; ++i) {
            if (fw_parser_push(&parser, host.tx[i], &stats)) {
                got = true;
                break;
            }
        }
        CHECK(got);
        CHECK(stats.command == (FW_CMD_STATS | 0x80U));
        CHECK(stats.length == 14U);
        CHECK(stats.payload[0] == 0U);
        CHECK(stats.payload[1] == 5U);
    }
}

/*
 * 周期上报与半帧超时测试。
 *
 * 上报：
 *   - 设置 1000ms 周期，推进假时钟 1000ms；
 *   - poll 应产生 0x90 事件帧，payload 全 0。
 *
 * 半帧超时：
 *   - 关闭上报；
 *   - 注入半个帧（A5 5A LEN SEQ CMD，无 CRC）；
 *   - 第一次 poll 不应计超时；
 *   - 推进 250ms 后 poll，partial_timeouts 应为 1。
 */
static void test_demo_report_and_timeout(void)
{
    fw_host_t host;
    fw_demo_app_t app;
    fw_demo_config_t config;
    const uint8_t report_payload[] = {0U, 0U, 0U};
    const uint8_t partial[] = {0xA5U, 0x5AU, 0x01U, 0x01U, 0x01U};

    demo_setup(&host, &app, &config);
    CHECK(fw_demo_set_report_period(&app, 1000U) == FW_OK);
    host_advance(&host, 1000U);
    CHECK(fw_demo_poll(&app) == FW_OK);
    expect_frame(&host, 0U, FW_EVENT_STATUS, report_payload, 3U);
    host.tx_length = 0U;

    CHECK(fw_demo_set_report_period(&app, 0U) == FW_OK);
    CHECK(host_serial_feed(&host, partial, sizeof(partial)) == sizeof(partial));
    CHECK(fw_demo_poll(&app) == FW_OK);
    CHECK(app.partial_timeouts == 0U);
    host_advance(&host, 250U);
    CHECK(fw_demo_poll(&app) == FW_OK);
    CHECK(app.partial_timeouts == 1U);
}

/*
 * 背压测试。
 *
 * 把 TX ring 限制为 2 字节，发送一个 PING 请求。
 * 回复帧远大于 2 字节，因此：
 *   - poll 返回 FW_EAGAIN；
 *   - app.tx_offset==2，说明只推进了 2 字节；
 *   - 恢复容量后再次 poll，应把剩余部分发完并清空 tx_length。
 *
 * 这验证 flush_tx 的“短写 + offset 续传”逻辑。
 */
static void test_demo_backpressure(void)
{
    fw_host_t host;
    fw_demo_app_t app;
    fw_demo_config_t config;

    demo_setup(&host, &app, &config);

    host_serial_set_write_limit(&host, 2U);
    feed_frame(&host, 7U, FW_CMD_PING, NULL, 0U);
    CHECK(fw_demo_poll(&app) == FW_EAGAIN);
    CHECK(app.tx_offset == 2U);
    CHECK(app.tx_length != 0U);

    host_serial_set_write_limit(&host, HOST_SERIAL_CAPACITY);
    CHECK(fw_demo_poll(&app) == FW_OK);
    CHECK(app.tx_length == 0U);
}

/* 依次运行所有测试；任一 CHECK 失败则返回 1。 */
int main(void)
{
    test_ring();
    test_protocol();
    test_device_registry();
    test_serial_pin();
    test_display();
    test_demo_basic();
    test_demo_report_and_timeout();
    test_demo_backpressure();
    if (failures != 0) {
        printf("%d test(s) failed\n", failures);
        return 1;
    }
    printf("all v2 host tests passed\n");
    return 0;
}
