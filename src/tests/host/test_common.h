/*
 * test_common.h - Host 测试共享的断言与协议帧辅助。
 *
 * 层级：src/tests/host
 * 职责：
 *   1. 提供 CHECK 宏，失败时打印位置并累加全局计数；
 *   2. 提供 feed_frame/expect_frame，供 framework 与应用测试复用。
 *
 * 上下文：单线程测试。
 * 依赖：host_devices.h、fw/protocol.h、stdio.h、string.h。
 */
#ifndef TEST_COMMON_H
#define TEST_COMMON_H

#include <stdio.h>
#include <string.h>
#include "host_devices.h"
#include "fw/protocol.h"

/* 全局失败计数；由 test_main.c 定义，所有测试文件共享。 */
extern int test_failures;

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
            ++test_failures;                                                  \
        }                                                                     \
    } while (0)

/*
 * 编码并注入一帧请求。
 *
 * 用真实的 fw_protocol_encode 生成字节，再写入假串口的 RX ring，
 * 这样测试同时覆盖编码器和 parser 的输入路径。
 */
static inline void feed_frame(fw_host_t *host, uint8_t sequence,
                              uint8_t command, const uint8_t *payload,
                              uint8_t length)
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
static inline void expect_frame(fw_host_t *host, uint8_t sequence,
                                uint8_t command, const uint8_t *payload,
                                uint8_t length)
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

#endif
