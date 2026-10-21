/*
 * app/rpc.h - 应用层 RPC 服务的状态与公开 API。
 *
 * 层级：src/app/rpc（公共头位于 src/app/include/app）
 * 职责：
 *   1. 把协议帧分发到具体命令（PING/LED/STATUS/STATS/TASK）；
 *   2. 维护 LED 的逻辑状态和统计计数；
 *   3. 提供非阻塞 TX 泵和周期状态上报；
 *   4. 通过 next_wait_ms 告诉 RPC 任务“下次最多睡多久”。
 *
 * 上下文：单拥有任务。结构体内部没有锁；如果多个任务调用 poll，
 *         必须由调用者串行化。
 *
 * 依赖：fw/dev.h、fw/serial.h、fw/pin.h、fw/clock.h、fw/protocol.h。
 *
 * 设计目标：
 *   app_rpc 不包含 rtthread.h，也不直接操作寄存器；
 *   串口、引脚和时间都通过 fw_dev_t / fw_clock_t 注入，
 *   因此同一份逻辑可以在 Host 测试中用假设备运行。
 */
#ifndef APP_RPC_H
#define APP_RPC_H

#include <stdbool.h>
#include "fw/dev.h"
#include "fw/serial.h"
#include "fw/pin.h"
#include "fw/clock.h"
#include "fw/protocol.h"
#include "fw/task.h"

/* 请求命令字（bit7=0）。 */
#define APP_RPC_CMD_PING 0x01U      /* 心跳：返回 "PONG" */
#define APP_RPC_CMD_LED_SET 0x02U   /* 设置 LED：payload[0] = 0/1 */
#define APP_RPC_CMD_STATUS 0x03U    /* 查询 LED 状态 */
#define APP_RPC_CMD_STATS 0x04U     /* 查询帧数/错误计数 */
#define APP_RPC_CMD_TASK_SUMMARY 0x05U /* 任务健康摘要 */
#define APP_RPC_CMD_TASK_DETAIL 0x06U  /* 按索引查询任务详情 */
#define APP_RPC_CMD_TASK_CONTROL 0x07U /* 挂起/恢复/改优先级 */

/* 主动上报事件（bit7=1）。 */
#define APP_RPC_EVENT_STATUS 0x90U

/* 任务控制动作（payload[1]）。 */
#define APP_RPC_TASK_SUSPEND 0U
#define APP_RPC_TASK_RESUME 1U
#define APP_RPC_TASK_SET_PRIORITY 2U

/* 任务回复的 payload 长度（不含帧头/CRC）。 */
#define APP_RPC_TASK_SUMMARY_LEN 13U
#define APP_RPC_TASK_DETAIL_LEN 31U

/*
 * 回复状态码；所有命令的 payload[0] 使用同一套值。
 * 0..3 是原有约定，4..7 是任务命令扩展。
 */
#define APP_RPC_ST_OK 0U
#define APP_RPC_ST_BAD_PARAM 1U
#define APP_RPC_ST_UNKNOWN_CMD 2U
#define APP_RPC_ST_IO 3U
#define APP_RPC_ST_NO_TASKS 4U
#define APP_RPC_ST_BAD_INDEX 5U
#define APP_RPC_ST_NOT_ALLOWED 6U
#define APP_RPC_ST_BUSY 7U

/* 默认上报周期：1000 ms。设为 0 可关闭周期上报。 */
#define APP_RPC_REPORT_PERIOD_MS 1000U

/*
 * 单次 poll 最多消费的 RX 字节数。
 * 限制预算可以避免持续有数据时 RPC 任务循环饿死其他工作（例如上报）。
 * 可通过编译定义覆盖，但必须落在 1..4096。
 */
#ifndef APP_RPC_RX_BUDGET
#define APP_RPC_RX_BUDGET 64U
#endif

/* TX 超时保留字段；当前 serial write 是非阻塞短写，不使用该值。 */
#ifndef APP_RPC_TX_TIMEOUT_MS
#define APP_RPC_TX_TIMEOUT_MS 20U
#endif

/*
 * 半帧超时：如果 parser 已经开始收帧，但超过该时间没有新字节，
 * 就复位 parser 并累加 partial_timeouts，避免一个噪声 A5 让解析器
 * 永远停在“等待 5A”的状态。
 */
#ifndef APP_RPC_PARTIAL_TIMEOUT_MS
#define APP_RPC_PARTIAL_TIMEOUT_MS 250U
#endif

/* rx_budget 的硬上限，防止配置成过大的值导致单次 poll 长时间占用 CPU。 */
#define APP_RPC_MAX_RX_BUDGET 4096U

#if (APP_RPC_RX_BUDGET < 1) || (APP_RPC_RX_BUDGET > APP_RPC_MAX_RX_BUDGET)
#error "APP_RPC_RX_BUDGET must be 1..APP_RPC_MAX_RX_BUDGET"
#endif

/*
 * 初始化配置。
 *
 * serial    - 必填；必须是 FW_DEV_TYPE_SERIAL；
 * pin       - 可选；NULL 表示没有 LED 输出，LED_SET 会返回错误码；
 * led_pin   - pin 设备上的引脚号（FW_PIN_NUMBER 编码）；
 * led_active_high - 逻辑“亮”对应的电平；
 * clock     - 必填；now_ms 不能为空；
 * tasks     - 可选；NULL 表示未启用任务健康命令。
 */
typedef struct {
    fw_dev_t *serial;
    fw_dev_t *pin;
    int led_pin;
    bool led_active_high;
    fw_clock_t clock;
    fw_task_manager_t *tasks;
} app_rpc_config_t;

/*
 * 应用运行状态。
 *
 * 前 6 个字段是初始化时保存的配置副本；
 * parser 是协议状态机；
 * tx/tx_length/tx_offset 组成“待发送帧 + 已写偏移”，
 *   用于处理 serial 的短写；
 * last_rx_ms 用于半帧超时；
 * frames/io_errors/partial_timeouts 是 STATS 命令的统计来源；
 * report_* 控制周期上报；
 * partial_timeout_ms/tx_timeout_ms/rx_budget 是运行参数；
 * report_sequence 是主动上报的序号；
 * led_known/led_requested_on 记录“是否成功写过”和“请求的逻辑状态”。
 */
typedef struct {
    fw_dev_t *serial;
    fw_dev_t *pin;
    int led_pin;
    bool led_active_high;
    fw_clock_t clock;
    fw_task_manager_t *tasks;

    fw_parser_t parser;

    uint8_t tx[FW_PROTOCOL_MAX_FRAME];
    size_t tx_length;
    size_t tx_offset;

    uint32_t last_rx_ms;
    uint32_t frames;
    uint32_t io_errors;
    uint32_t partial_timeouts;

    uint32_t report_period_ms;
    uint32_t last_report_ms;
    uint32_t partial_timeout_ms;
    uint32_t tx_timeout_ms;
    uint32_t rx_budget;

    uint8_t report_sequence;
    bool led_known;
    bool led_requested_on;
} app_rpc_t;

/*
 * 初始化应用。
 *
 * 要求 serial 非空且类型正确、clock.now_ms 非空；
 * 会把结构体清零，然后保存配置、初始化 parser、设置默认参数。
 * 返回 FW_OK 或 FW_EINVAL。
 */
fw_status_t app_rpc_init(app_rpc_t *app, const app_rpc_config_t *config);

/* 调整半帧超时、TX 超时和 RX 预算；参数非法返回 FW_EINVAL。 */
fw_status_t app_rpc_set_limits(app_rpc_t *app, uint32_t partial_timeout_ms,
                               uint32_t tx_timeout_ms, uint32_t rx_budget);

/* 设置周期上报间隔；0 表示关闭。会重置 last_report_ms。 */
fw_status_t app_rpc_set_report_period(app_rpc_t *app, uint32_t period_ms);

/* next_wait_ms 的“无定时任务”返回值。 */
#define APP_RPC_WAIT_FOREVER UINT32_MAX

/*
 * 计算 RPC 任务下一次最多可以等待多久（毫秒）。
 *
 * 考虑三种约束：
 *   - 有未发完的 TX 帧 -> 返回 1，尽快回来继续泵；
 *   - 周期上报到期 -> 返回剩余时间（可能为 0）；
 *   - 半帧超时 -> 返回剩余时间。
 * 取三者最小值；没有约束时返回 APP_RPC_WAIT_FOREVER。
 *
 * RPC 任务把它传给 fw_serial_wait_rx，实现“有数据立即处理，
 * 没数据时按最近定时器睡眠”，并在 app_task 中 clamp 到心跳上限。
 */
uint32_t app_rpc_next_wait_ms(const app_rpc_t *app);

/*
 * 处理一轮工作：
 *   1. 如果有未发完的 TX，先继续发送；
 *   2. 在 RX 预算内逐字节读取并喂给 parser；
 *   3. 每解析出一帧就分发命令并尽快回复；
 *   4. 检查半帧超时；
 *   5. 检查周期上报。
 *
 * 返回 FW_OK 或底层读写错误；调用者通常在 RPC 任务循环中忽略返回值，
 * 由应用自行记录错误计数。
 */
fw_status_t app_rpc_poll(app_rpc_t *app);

#endif
