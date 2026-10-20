/*
 * demo_app.c - 示例命令分发、TX 泵和轮询调度。
 *
 * 层级：src/framework/app（示例应用）
 * 职责：
 *   1. 提供命令表，把请求帧映射到处理函数；
 *   2. 通过 pin 设备控制 LED；
 *   3. 维护 STATS 计数和周期状态上报；
 *   4. 用“短写 + offset”方式非阻塞地发送回复。
 *
 * 上下文：单拥有任务。
 * 依赖：fw/demo_app.h、string.h。
 *
 * 处理流程：
 *
 *   UART RX ring -> fw_dev_read 1 字节 -> fw_parser_push
 *        -> 完整帧 -> handle_message -> 命令处理 -> 编码回复
 *        -> tx[] -> flush_tx -> serial TX ring -> UART ISR
 *
 * 为什么一次只读 1 字节：
 *   parser 是逐字节状态机，读 1 字节可以精确控制 RX 预算，
 *   也避免为了“可能只有半帧”而在栈上准备大缓冲。
 *   串口 class 的 ring 已经承担了批量缓冲的职责。
 */
#include "fw/demo_app.h"
#include <string.h>

/*
 * 命令处理函数类型。
 *
 * request 是解析出的请求帧；reply 由调用者预先填好 sequence/command，
 * 处理函数只需要填写 payload 和 length 即可。
 */
typedef void (*fw_demo_handler_t)(fw_demo_app_t *app,
                                  const fw_message_t *request,
                                  fw_message_t *reply);

/* 命令表项：命令字 + 处理函数。 */
typedef struct {
    uint8_t command;
    fw_demo_handler_t handler;
} fw_demo_command_t;

/*
 * 小端写入 32 位整数。
 *
 * 不直接 memcpy uint32_t，是为了避免依赖主机字节序；
 * 协议明确规定多字节字段小端，这里逐字节拆分。
 */
static void put_u32_le(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8U);
    out[2] = (uint8_t)(value >> 16U);
    out[3] = (uint8_t)(value >> 24U);
}

/*
 * PING 命令。
 *
 * 请求必须没有 payload；回复 payload[0]=0（成功），
 * 后面跟 "PONG" 4 个字节。reply->length 在进入处理函数前
 * 已经被 handle_message 置为 1（状态字节），这里改成 5。
 */
static void cmd_ping(fw_demo_app_t *app, const fw_message_t *request,
                     fw_message_t *reply)
{
    (void)app;
    if (request->length != 0U) {
        reply->payload[0] = 1U; /* 1 = 参数错误 */
        return;
    }
    reply->length = 5U;
    memcpy(&reply->payload[1], "PONG", 4U);
}

/*
 * LED_SET 命令。
 *
 * 请求 payload 必须正好 1 字节，且只能是 0/1。
 * 逻辑状态与物理电平通过 led_active_high 转换：
 *   value = (请求亮/灭 == 高有效) ? 1 : 0
 * 例如低有效 LED，请求“亮”时应输出 0。
 *
 * 只有写引脚成功后才更新 led_known/led_requested_on；
 * 写失败返回错误码 3（IO 错误），避免上报一个并不存在的状态。
 */
static void cmd_led_set(fw_demo_app_t *app, const fw_message_t *request,
                        fw_message_t *reply)
{
    int result;
    uint8_t value;

    if (request->length != 1U || request->payload[0] > 1U) {
        reply->payload[0] = 1U;
        return;
    }
    if (app->pin == NULL) {
        reply->payload[0] = 3U; /* 3 = 没有可用输出设备 */
        return;
    }

    value = (request->payload[0] != 0U) == app->led_active_high ? 1U : 0U;
    result = fw_pin_write(app->pin, app->led_pin, value);
    if (result >= 0) {
        app->led_known = true;
        app->led_requested_on = request->payload[0] != 0U;
    } else {
        reply->payload[0] = 3U;
    }
}

/*
 * STATUS 命令。
 *
 * 回复 3 字节：
 *   payload[0] = 0（状态码）
 *   payload[1] = 是否成功写过 LED（0/1）
 *   payload[2] = 最近一次请求的逻辑状态（0/1）
 *
 * 注意：[1] 和 [2] 可能不一致：例如成功点亮后又断电重启，
 * 软件知道“请求过亮”，但无法确认物理状态。
 */
static void cmd_status(fw_demo_app_t *app, const fw_message_t *request,
                       fw_message_t *reply)
{
    if (request->length != 0U) {
        reply->payload[0] = 1U;
        return;
    }
    reply->length = 3U;
    reply->payload[1] = app->led_known ? 1U : 0U;
    reply->payload[2] = app->led_requested_on ? 1U : 0U;
}

/*
 * STATS 命令。
 *
 * 回复 14 字节：
 *   [0]      状态码 0
 *   [1..4]   已处理帧数（小端）
 *   [5..8]   读写错误数
 *   [9..12]  半帧超时数
 *   [13]     主动上报序号
 *
 * 计数器是 32 位，运行很久后自然回绕；对于诊断用途足够。
 */
static void cmd_stats(fw_demo_app_t *app, const fw_message_t *request,
                      fw_message_t *reply)
{
    if (request->length != 0U) {
        reply->payload[0] = 1U;
        return;
    }
    reply->length = 14U;
    put_u32_le(&reply->payload[1], app->frames);
    put_u32_le(&reply->payload[5], app->io_errors);
    put_u32_le(&reply->payload[9], app->partial_timeouts);
    reply->payload[13] = app->report_sequence;
}

/*
 * 命令表。
 *
 * 线性查找，表很小（4 项），没有必要用哈希或二分。
 * 新增命令时在这里追加一行，并确保命令字唯一。
 */
static const fw_demo_command_t command_table[] = {
    { FW_CMD_PING, cmd_ping },
    { FW_CMD_LED_SET, cmd_led_set },
    { FW_CMD_STATUS, cmd_status },
    { FW_CMD_STATS, cmd_stats }
};

/*
 * 分发一条完整请求。
 *
 * 步骤：
 *   1. 忽略 bit7=1 的帧（回复/事件不应该被设备再次处理，避免回环）；
 *   2. 预先填好回复的 sequence、command（请求 | 0x80）、length=1；
 *   3. 在命令表中查找并调用处理函数；
 *   4. 未找到命令时 payload[0]=2（未知命令）；
 *   5. 把回复编码进 app->tx，并重置 tx_offset。
 *
 * 处理函数可以直接修改 reply->length 和 payload，
 * 但状态字节 payload[0] 默认已经由这里初始化为 0。
 */
static fw_status_t handle_message(fw_demo_app_t *app, const fw_message_t *request)
{
    fw_message_t reply = {0};
    size_t i;
    size_t count = sizeof(command_table) / sizeof(command_table[0]);

    /* 只处理请求帧；bit7 置位的帧是本机发出的回复/事件。 */
    if ((request->command & 0x80U) != 0U) return FW_OK;

    reply.sequence = request->sequence;
    reply.command = (uint8_t)(request->command | 0x80U);
    reply.length = 1U; /* 默认只有状态字节 0 */

    for (i = 0U; i < count; ++i) {
        if (command_table[i].command == request->command) {
            command_table[i].handler(app, request, &reply);
            break;
        }
    }
    if (i == count) reply.payload[0] = 2U; /* 2 = 未知命令 */

    app->tx_offset = 0U;
    return fw_protocol_encode(&reply, app->tx, sizeof(app->tx), &app->tx_length);
}

/*
 * 把 tx[] 中尚未发送的部分推入串口 TX ring。
 *
 * serial_write 是非阻塞短写：
 *   - 返回 >0 表示 ring 接收了多少字节；
 *   - 返回 0 表示 ring 满，暂时无法接收；
 *   - 返回 <0 表示硬件/状态错误。
 *
 * 用 tx_offset 记录已推进的位置，下次 poll 从断点继续，
 * 不会重复发送已经入队的数据。
 *
 * 返回值：
 *   FW_OK     本帧全部入队；
 *   FW_EAGAIN 还有剩余，需要稍后重试；
 *   其他负值  底层错误。
 */
static fw_status_t flush_tx(fw_demo_app_t *app)
{
    int accepted;
    if (app->tx_offset >= app->tx_length) {
        app->tx_length = 0U;
        app->tx_offset = 0U;
        return FW_OK;
    }

    accepted = fw_dev_write(app->serial, 0, &app->tx[app->tx_offset],
                            app->tx_length - app->tx_offset, 0U);
    if (accepted < 0) {
        ++app->io_errors;
        return (fw_status_t)accepted;
    }
    app->tx_offset += (size_t)accepted;

    if (app->tx_offset == app->tx_length) {
        app->tx_length = 0U;
        app->tx_offset = 0U;
        return FW_OK;
    }
    return FW_EAGAIN;
}

/*
 * 初始化应用。
 *
 * 校验：
 *   - app/config 非空；
 *   - serial 非空且类型是 SERIAL；
 *   - clock.now_ms 非空（否则无法做超时和周期判断）。
 *
 * 然后清零整个状态、保存配置、初始化 parser，
 * 并把超时/预算设置为头文件默认值。
 * 周期上报默认关闭（report_period_ms=0），由 main 显式设置。
 */
fw_status_t fw_demo_init(fw_demo_app_t *app, const fw_demo_config_t *config)
{
    if (app == NULL || config == NULL || config->serial == NULL ||
        config->clock.now_ms == NULL) return FW_EINVAL;
    if (config->serial->type != FW_DEV_TYPE_SERIAL) return FW_EINVAL;

    memset(app, 0, sizeof(*app));
    app->serial = config->serial;
    app->pin = config->pin;
    app->led_pin = config->led_pin;
    app->led_active_high = config->led_active_high;
    app->clock = config->clock;
    app->partial_timeout_ms = FW_DEMO_PARTIAL_TIMEOUT_MS;
    app->tx_timeout_ms = FW_DEMO_TX_TIMEOUT_MS;
    app->rx_budget = FW_DEMO_RX_BUDGET;
    fw_parser_init(&app->parser);
    return FW_OK;
}

/*
 * 调整运行参数。
 *
 * partial_timeout_ms 允许为 0（关闭半帧超时），但必须能放进 int32，
 * 因为上层可能用有符号时间差比较；
 * rx_budget 必须在 1..FW_DEMO_MAX_RX_BUDGET，避免一次 poll 占用过久。
 */
fw_status_t fw_demo_set_limits(fw_demo_app_t *app, uint32_t partial_timeout_ms,
                               uint32_t tx_timeout_ms, uint32_t rx_budget)
{
    if (app == NULL || partial_timeout_ms > INT32_MAX ||
        rx_budget == 0U || rx_budget > FW_DEMO_MAX_RX_BUDGET) return FW_EINVAL;
    app->partial_timeout_ms = partial_timeout_ms;
    app->tx_timeout_ms = tx_timeout_ms;
    app->rx_budget = rx_budget;
    return FW_OK;
}

/*
 * 设置周期上报间隔。
 *
 * period_ms=0 表示关闭上报；设置后立即记录当前时间，
 * 避免刚设置就立刻触发一次上报。
 */
fw_status_t fw_demo_set_report_period(fw_demo_app_t *app, uint32_t period_ms)
{
    if (app == NULL || app->clock.now_ms == NULL || period_ms > INT32_MAX)
        return FW_EINVAL;
    app->report_period_ms = period_ms;
    app->last_report_ms = app->clock.now_ms(app->clock.ctx);
    return FW_OK;
}

/*
 * 构造并发送一次 0x90 状态上报。
 *
 * payload：
 *   [0] 状态码 0
 *   [1] LED 是否已知
 *   [2] LED 请求状态
 *
 * 上报序号使用独立的 report_sequence，和请求/回复的 sequence 分开。
 * 发送仍然走 flush_tx；如果 TX ring 满，会返回 FW_EAGAIN，
 * 下一轮 poll 继续发送。
 */
static fw_status_t report_status(fw_demo_app_t *app, uint32_t now)
{
    fw_message_t report = {0};
    fw_status_t result;

    report.command = FW_EVENT_STATUS;
    report.sequence = app->report_sequence;
    report.length = 3U;
    report.payload[1] = app->led_known ? 1U : 0U;
    report.payload[2] = app->led_requested_on ? 1U : 0U;

    result = fw_protocol_encode(&report, app->tx, sizeof(app->tx), &app->tx_length);
    if (result != FW_OK) return result;

    app->tx_offset = 0U;
    ++app->report_sequence;
    app->last_report_ms = now;
    return flush_tx(app);
}

/*
 * 计算下一次可等待的毫秒数。
 *
 * 优先级：
 *   1. 有未发完的 TX -> 1ms（尽快回来泵数据）；
 *   2. 周期上报 -> 距离下次上报的剩余时间；
 *   3. 半帧超时 -> 距离超时的剩余时间；
 *   4. 都没有 -> FW_DEMO_WAIT_FOREVER，让主循环睡到串口有数据。
 *
 * 使用 fw_elapsed_ms 处理 32 位 tick 回绕。
 * 返回 0 表示“已经到期，应立即处理”。
 */
uint32_t fw_demo_next_wait_ms(const fw_demo_app_t *app)
{
    uint32_t now;
    uint32_t wait = FW_DEMO_WAIT_FOREVER;
    uint32_t elapsed;
    uint32_t remaining;

    if (app == NULL || app->clock.now_ms == NULL) return 0U;
    if (app->tx_length != 0U) return 1U;

    now = app->clock.now_ms(app->clock.ctx);

    if (app->report_period_ms != 0U) {
        elapsed = fw_elapsed_ms(now, app->last_report_ms);
        remaining = elapsed >= app->report_period_ms ? 0U :
                    app->report_period_ms - elapsed;
        if (remaining < wait) wait = remaining;
    }

    if (app->partial_timeout_ms != 0U && fw_parser_partial(&app->parser)) {
        elapsed = fw_elapsed_ms(now, app->last_rx_ms);
        remaining = elapsed >= app->partial_timeout_ms ? 0U :
                    app->partial_timeout_ms - elapsed;
        if (remaining < wait) wait = remaining;
    }
    return wait;
}

/*
 * 处理一轮工作。
 *
 * 顺序：
 *   1. 如果上一帧还没发完，先继续 flush，避免新回复覆盖 tx[]；
 *   2. 在 rx_budget 内逐字节读取：
 *        - 0 字节表示暂时没有数据，结束本轮；
 *        - 负值表示底层错误，复位 parser、计数并返回；
 *        - 每读到一个字节就更新 last_rx_ms；
 *   3. 每解析出一帧：
 *        - frames++；
 *        - 分发命令；
 *        - 如果产生了回复，立即 flush；若短写则留到下一轮；
 *   4. 如果本轮没有任何输入，且 parser 处于半帧状态超过阈值，
 *      复位 parser 并累加 partial_timeouts；
 *   5. 如果周期上报到期，构造并发送 0x90。
 *
 * 返回：
 *   FW_OK     本轮正常完成；
 *   其他负值  底层读写/编码错误。
 *
 * 注意：report_status 的返回值直接返回给调用者；
 * 如果它返回 FW_EAGAIN，表示上报数据还在 tx[] 中等待下一轮，
 * 并不代表致命错误。
 */
fw_status_t fw_demo_poll(fw_demo_app_t *app)
{
    unsigned budget;
    uint8_t byte;
    uint32_t now;
    uint32_t consumed = 0U;
    int received;
    fw_status_t result;
    fw_message_t message;

    if (app == NULL || app->clock.now_ms == NULL) return FW_EINVAL;
    now = app->clock.now_ms(app->clock.ctx);

    /* 上一帧尚未完全入队：先推进它，避免覆盖 tx[]。 */
    if (app->tx_length != 0U) return flush_tx(app);

    for (budget = 0U; budget < app->rx_budget; ++budget) {
        received = fw_dev_read(app->serial, 0, &byte, 1U, 0U);
        if (received == 0) break; /* 非阻塞读：当前没有更多数据 */
        if (received < 0) {
            /* 底层错误通常意味着 ring/硬件状态异常，复位解析器重新同步。 */
            fw_parser_reset(&app->parser);
            ++app->io_errors;
            return (fw_status_t)received;
        }
        ++consumed;
        app->last_rx_ms = now;

        if (fw_parser_push(&app->parser, byte, &message)) {
            ++app->frames;
            result = handle_message(app, &message);
            if (result != FW_OK) return result;

            if (app->tx_length != 0U) {
                result = flush_tx(app);
                if (result != FW_OK) return result;
                break; /* 回复已入队，先处理发送，剩余 RX 留到下一轮 */
            }
        }
    }

    /* 半帧超时：防止一个噪声前导码让 parser 永远停在收帧状态。 */
    if (consumed == 0U && app->partial_timeout_ms != 0U &&
        fw_parser_partial(&app->parser) &&
        fw_elapsed_ms(now, app->last_rx_ms) >= app->partial_timeout_ms) {
        fw_parser_reset(&app->parser);
        ++app->partial_timeouts;
    }

    /* 周期上报；到期后立即发送，短写由下一轮 poll 继续。 */
    if (app->report_period_ms != 0U &&
        fw_elapsed_ms(now, app->last_report_ms) >= app->report_period_ms)
        return report_status(app, now);
    return FW_OK;
}
