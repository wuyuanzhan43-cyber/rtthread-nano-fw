/*
 * fw/protocol.h - 带 CRC-16 的二进制帧协议。
 *
 * 层级：src/framework/include/fw（实现位于 src/framework/components/protocol）
 * 职责：
 *   1. 把 fw_message_t 编码成线上字节；
 *   2. 从任意字节流中逐字节恢复完整帧；
 *   3. 通过 CRC 和长度上限拒绝损坏/越界数据。
 *
 * 上下文：
 *   parser 状态由单个任务拥有，无内部锁；如果多个任务共享同一个
 *   parser，必须由调用者串行化。
 *
 * 依赖：fw/status.h、stdbool.h、stddef.h、stdint.h。
 *
 * 帧格式（多字节字段均为小端）：
 *
 *   +------+------+-----+-----+-----+---------+--------+--------+
 *   |  A5  |  5A  | LEN | SEQ | CMD | PAYLOAD | CRC_LO | CRC_HI |
 *   +------+------+-----+-----+-----+---------+--------+--------+
 *
 *   LEN     - payload 长度，0..FW_PROTOCOL_MAX_PAYLOAD；
 *   SEQ     - 序号，回复沿用请求值；
 *   CMD     - 命令字；bit7=0 请求，bit7=1 回复/事件；
 *   CRC     - CRC-16/CCITT-FALSE，覆盖 LEN、SEQ、CMD、PAYLOAD，
 *             不覆盖 A5 5A 前导码。
 *
 * 设计取舍：
 *   - 固定最大 payload 32 字节，parser 和消息对象都可以静态分配；
 *   - 逐字节状态机，不要求一次收到完整帧，适合串口 IT 路径；
 *   - 收到非法长度或 CRC 错误时只计数并重新寻找前导码，
 *     不会让解析器卡死，也不会阻塞后续合法帧。
 */
#ifndef FW_PROTOCOL_H
#define FW_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "fw/status.h"

/* 单帧最大 payload；编码缓冲区大小由此推导。 */
#define FW_PROTOCOL_MAX_PAYLOAD 32U

/* 最大帧长 = 前导码 2 + LEN/SEQ/CMD 3 + payload + CRC 2 = payload + 7。 */
#define FW_PROTOCOL_MAX_FRAME (FW_PROTOCOL_MAX_PAYLOAD + 7U)

/*
 * 一条完整消息（解码结果或待编码内容）。
 *
 * sequence - 用于把回复和请求关联起来；
 * command  - 命令字；最高位表示方向（0=请求，1=回复/事件）；
 * length   - payload 有效字节数；
 * payload  - 复制存储，不引用 parser 的内部缓冲，调用者可以安全保存。
 */
typedef struct {
    uint8_t sequence;
    uint8_t command;
    uint8_t length;
    uint8_t payload[FW_PROTOCOL_MAX_PAYLOAD];
} fw_message_t;

/*
 * 逐字节解析器。
 *
 * state        - 内部状态机位置，外部应视为不透明；
 * body         - LEN + SEQ + CMD + payload 的临时缓冲；
 *                CRC 也覆盖这一段，因此单独保存便于统一计算；
 * used         - body 当前已填充字节数；
 * received_crc - 线上收到的 CRC，先低字节后高字节拼装；
 * bad_crc      - CRC 错误累计次数，用于诊断；
 * bad_length   - 长度非法累计次数，用于诊断。
 *
 * 诊断计数器在 fw_parser_reset 后保留，只有 fw_parser_init 会清零，
 * 这样半帧超时复位不会丢失整条流的错误统计。
 */
typedef struct {
    uint8_t state;
    uint8_t body[FW_PROTOCOL_MAX_PAYLOAD + 3U];
    size_t used;
    uint16_t received_crc;
    uint32_t bad_crc;
    uint32_t bad_length;
} fw_parser_t;

/* 初始化 parser：状态归零、计数器清零；parser 为 NULL 时安全无操作。 */
void fw_parser_init(fw_parser_t *parser);

/*
 * 只复位未完成帧的状态：
 *   - state 回到 SEEK_A5；
 *   - used 和 received_crc 清零；
 *   - bad_crc/bad_length 保留（它们描述整条流，不属于当前半帧）。
 */
void fw_parser_reset(fw_parser_t *parser);

/* 当前是否已开始一帧但尚未完成；用于半帧超时判断。 */
bool fw_parser_partial(const fw_parser_t *parser);

/*
 * 送入一个字节。
 *
 * 返回：
 *   true  - 正好凑齐一帧且 CRC 正确，payload 已复制到 *message；
 *   false - 尚未完成，或该字节被当作噪声/错误消费。
 *
 * 同步策略：
 *   遇到非法长度或 CRC 错误时回到 SEEK_A5；如果出错字节本身是 A5，
 *   会把它当作下一帧前导码的首字节继续解析，避免漏掉紧邻的合法帧。
 */
bool fw_parser_push(fw_parser_t *parser, uint8_t byte, fw_message_t *message);

/*
 * CRC-16/CCITT-FALSE。
 * 参数：init=0xFFFF、poly=0x1021、不反射、xorout=0。
 * 逐位实现，代码小且结果确定；一帧最多 39 字节，不追求吞吐。
 */
uint16_t fw_crc16(const uint8_t *data, size_t length);

/*
 * 编码一条消息。
 *
 * 参数：
 *   message  - 待编码内容；
 *   output   - 输出缓冲；
 *   capacity - output 容量，必须 >= message->length + 7；
 *   length   - 输出实际帧长；失败时置 0。
 *
 * 返回 FW_OK 或 FW_EINVAL/FW_EOVERFLOW。
 * payload 会被复制，编码完成后不保留任何指针。
 */
fw_status_t fw_protocol_encode(const fw_message_t *message, uint8_t *output,
                               size_t capacity, size_t *length);

#endif
