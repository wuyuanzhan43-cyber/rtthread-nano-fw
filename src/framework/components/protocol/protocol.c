/*
 * protocol.c - CRC-16 与逐字节帧解析。
 *
 * 层级：src/framework/components/protocol
 * 职责：
 *   1. 提供确定性的 CRC-16/CCITT-FALSE；
 *   2. 用 6 状态的状态机从任意字节流中恢复帧；
 *   3. 拒绝超长帧和 CRC 错误，并在错误后重新同步。
 *
 * 上下文：parser 由单拥有任务使用；无内部锁、无动态分配。
 * 依赖：fw/protocol.h、string.h。
 *
 * 状态机：
 *
 *   SEEK_A5 -> SEEK_5A -> READ_LENGTH -> READ_BODY
 *           -> READ_CRC_LO -> READ_CRC_HI -> SEEK_A5
 *
 * 每个状态只消费一个字节，因此调用者可以每次从串口 ring 取 1 字节
 * 喂进来，不需要先攒够一整帧，也不需要知道帧边界。
 */
#include "fw/protocol.h"
#include <string.h>

/*
 * parser 状态按线路顺序排列。
 *
 * 枚举保持私有：外部只能通过 fw_parser_partial() 判断“是否正在收帧”，
 * 不依赖内部状态编号，方便以后扩展状态机。
 */
enum { SEEK_A5, SEEK_5A, READ_LENGTH, READ_BODY, READ_CRC_LO, READ_CRC_HI };

/* =========================================================================
 * CRC 校验
 * ========================================================================= */

/*
 * CRC-16/CCITT-FALSE。
 *
 * 算法参数：
 *   init   = 0xFFFF
 *   poly   = 0x1021
 *   refin  = false
 *   refout = false
 *   xorout = 0
 *
 * 实现方式：
 *   每次把当前字节异或到 CRC 高 8 位，然后逐位左移；
 *   最高位为 1 时异或多项式。逐位实现比查表慢，但代码小、
 *   不需要 512 字节常量表，且一帧最多 39 字节，完全够用。
 *
 * 注意：计算时使用了 uint32_t 中间量，避免 uint16_t 左移时
 * 在部分编译器上产生未定义的提升行为；最终截断回 16 位。
 */
uint16_t fw_crc16(const uint8_t *data, size_t length)
{
    uint16_t crc = UINT16_C(0xffff);
    size_t i;
    unsigned bit;

    for (i = 0U; i < length; ++i) {
        crc ^= (uint16_t)((uint16_t)data[i] << 8U);
        for (bit = 0U; bit < 8U; ++bit) {
            /* 逐位实现让代码更小、结果确定；一帧最多 39 字节，
             * 这里不追求速度。 */
            crc = (uint16_t)((crc & UINT16_C(0x8000)) != 0U
                  ? ((uint32_t)crc << 1U) ^ UINT32_C(0x1021)
                  : (uint32_t)crc << 1U);
        }
    }
    return crc;
}

/* =========================================================================
 * 解析器
 * ========================================================================= */

/*
 * 初始化 parser。
 *
 * memset 会把 state 置 0，正好对应 SEEK_A5（枚举第一个值），
 * 同时清零 used、received_crc 和两个诊断计数器。
 */
void fw_parser_init(fw_parser_t *parser)
{
    if (parser != NULL) memset(parser, 0, sizeof(*parser));
}

/*
 * 复位未完成帧。
 *
 * 与 init 的区别：不清零 bad_crc/bad_length。
 * 这两个计数描述整条字节流的质量，半帧超时或 CRC 错误后仍然有意义，
 * 因此只重置与当前帧相关的字段。
 */
void fw_parser_reset(fw_parser_t *parser)
{
    if (parser != NULL) {
        parser->state = SEEK_A5;
        parser->used = 0U;
        parser->received_crc = 0U;
        /* bad_crc/bad_length 故意保留：它们描述整条流，不属于当前半帧。 */
    }
}

/* 只要不在“寻找 A5”状态，就说明已经开始了半帧。 */
bool fw_parser_partial(const fw_parser_t *parser)
{
    return parser != NULL && parser->state != SEEK_A5;
}

/*
 * 逐字节推进状态机。
 *
 * 各状态的处理：
 *
 * SEEK_A5:
 *   等待 0xA5。其他字节直接丢弃（噪声）。
 *
 * SEEK_5A:
 *   期待 0x5A。若又收到 0xA5，保持在本状态（可能是新帧前导码）；
 *   其他字节回到 SEEK_A5。
 *
 * READ_LENGTH:
 *   第一字节是 LEN。超过最大 payload 时计数并复位；
 *   如果这个非法字节本身是 0xA5，把它当作下一帧前导码继续。
 *   合法时写入 body[0]，used=1。
 *
 * READ_BODY:
 *   继续填充 body。body 结构是 LEN, SEQ, CMD, payload...
 *   当 used == LEN + 3 时，说明 SEQ/CMD/payload 都收齐了，
 *   进入 CRC 低字节状态。
 *
 * READ_CRC_LO / READ_CRC_HI:
 *   小端拼装 CRC，然后与本地计算的 CRC 比较：
 *     匹配 -> 填充 message 并返回 true；
 *     不匹配 -> bad_crc++ 并复位。
 *
 * 安全性：
 *   READ_BODY 的写入位置由 LEN 控制，而 LEN 已在 READ_LENGTH
 *   检查过 <= FW_PROTOCOL_MAX_PAYLOAD，body 容量为 payload+3，
 *   因此不会越界。
 *
 * 返回值只表示“是否产出一帧”，错误通过计数器和状态复位体现，
 * 调用者不需要为噪声字节写错误分支。
 */
bool fw_parser_push(fw_parser_t *parser, uint8_t byte, fw_message_t *message)
{
    if (parser == NULL || message == NULL) return false;

    switch (parser->state) {
    case SEEK_A5:
        if (byte == 0xa5U) parser->state = SEEK_5A;
        break;

    case SEEK_5A:
        /* 重复的 A5 可能是新帧前导码，不应视为错误。 */
        parser->state = byte == 0x5aU ? READ_LENGTH :
                        (byte == 0xa5U ? SEEK_5A : SEEK_A5);
        break;

    case READ_LENGTH:
        if (byte > FW_PROTOCOL_MAX_PAYLOAD) {
            ++parser->bad_length;
            fw_parser_reset(parser);
            /* 如果被拒绝的长度字节本身是 A5，它也可能是下一帧前导码的首字节。 */
            if (byte == 0xa5U) parser->state = SEEK_5A;
        } else {
            parser->body[0] = byte;
            parser->used = 1U;
            parser->state = READ_BODY;
        }
        break;

    case READ_BODY:
        parser->body[parser->used++] = byte;
        /* body[0] 是 LEN；再加上 SEQ 和 CMD 两个字节。 */
        if (parser->used == (size_t)parser->body[0] + 3U) {
            parser->state = READ_CRC_LO;
        }
        break;

    case READ_CRC_LO:
        parser->received_crc = byte;
        parser->state = READ_CRC_HI;
        break;

    case READ_CRC_HI:
        parser->received_crc |= (uint16_t)((uint16_t)byte << 8U);
        if (parser->received_crc == fw_crc16(parser->body, parser->used)) {
            /* CRC 正确：把 body 拆分到消息对象，然后复位准备下一帧。 */
            message->length = parser->body[0];
            message->sequence = parser->body[1];
            message->command = parser->body[2];
            memcpy(message->payload, &parser->body[3], message->length);
            fw_parser_reset(parser);
            return true;
        }
        /* CRC 错误：计数后重新寻找前导码，不把坏帧交给上层。 */
        ++parser->bad_crc;
        fw_parser_reset(parser);
        break;

    default:
        /* 状态损坏也可恢复：重新寻找前导码。 */
        fw_parser_reset(parser);
        break;
    }
    return false;
}

/* =========================================================================
 * 编码
 * ========================================================================= */

/*
 * 把消息编码成完整帧。
 *
 * 步骤：
 *   1. 校验 message/output/length，payload 不得超限；
 *   2. 计算总长 = payload + 7，检查输出容量；
 *   3. 写入前导码 A5 5A、LEN、SEQ、CMD、payload；
 *   4. 对 LEN..payload 计算 CRC 并小端写入末尾；
 *   5. 返回总长。
 *
 * 失败时把 *length 置 0，避免调用者误用旧值。
 * output 可能与 message->payload 重叠吗？当前调用者不会这样用；
 * 函数内部先写前导码再 memcpy payload，若重叠会有覆盖风险，
 * 因此约定输出缓冲区必须独立。
 */
fw_status_t fw_protocol_encode(const fw_message_t *message, uint8_t *output,
                               size_t capacity, size_t *length)
{
    uint16_t crc;
    size_t total;

    if (length == NULL) return FW_EINVAL;
    *length = 0U;
    if (message == NULL || output == NULL ||
        message->length > FW_PROTOCOL_MAX_PAYLOAD) return FW_EINVAL;

    total = (size_t)message->length + 7U;
    if (capacity < total) return FW_EOVERFLOW;

    output[0] = 0xa5U;
    output[1] = 0x5aU;
    output[2] = message->length;
    output[3] = message->sequence;
    output[4] = message->command;
    memcpy(&output[5], message->payload, message->length);

    /* CRC 覆盖 LEN 到 payload 末尾；起始位置是 output[2]。 */
    crc = fw_crc16(&output[2], (size_t)message->length + 3U);
    output[total - 2U] = (uint8_t)crc;
    output[total - 1U] = (uint8_t)(crc >> 8U);
    *length = total;
    return FW_OK;
}
