/*
 * test_app_rpc.c - 应用层 RPC 服务 Host 测试。
 *
 * 层级：src/tests/host
 * 职责：
 *   1. 验证 PING/LED_SET/STATUS/STATS 命令与回复；
 *   2. 验证 0x90 周期上报、半帧超时和 TX 背压；
 *   3. 验证 0x05/0x06/0x07 任务健康查询与控制；
 *   4. 通过假设备走真实的 fw_dev_* 路径。
 *
 * 上下文：单线程。
 * 依赖：test_common.h、host_tasks.h、app/rpc.h、fw/status.h。
 */
#include "test_common.h"
#include "host_tasks.h"
#include "app/rpc.h"
#include "fw/status.h"

/*
 * 初始化 RPC 测试环境。
 *
 * 注册假设备并把 serial/pin/LED/clock 填入配置；
 * 设置与 main.c 相同的限制参数（250/20/64）。
 */
static void rpc_setup(fw_host_t *host, app_rpc_t *rpc,
                      app_rpc_config_t *config)
{
    host_init(host);
    host_register_devices(host);
    config->serial = &host->serial.dev;
    config->pin = &host->pin.dev;
    config->led_pin = HOST_LED_PIN;
    config->led_active_high = true;
    config->clock = host_clock(host);
    config->tasks = NULL;
    CHECK(app_rpc_init(rpc, config) == FW_OK);
    CHECK(app_rpc_set_limits(rpc, 250U, 20U, 64U) == FW_OK);
}

/*
 * 基本命令测试。
 *
 * 依次发送 PING、LED_SET、STATUS、未知命令、STATS，
 * 检查回复帧的 sequence/command/payload、LED 电平和统计值。
 */
static void test_app_rpc_basic(void)
{
    fw_host_t host;
    app_rpc_t rpc;
    app_rpc_config_t config;
    const uint8_t ping_reply[] = {0U, 'P', 'O', 'N', 'G'};
    const uint8_t led_on[] = {1U};
    const uint8_t led_reply[] = {0U};
    const uint8_t status_reply[] = {0U, 1U, 1U};
    const uint8_t unknown_reply[] = {2U};

    rpc_setup(&host, &rpc, &config);

    feed_frame(&host, 1U, APP_RPC_CMD_PING, NULL, 0U);
    CHECK(app_rpc_poll(&rpc) == FW_OK);
    expect_frame(&host, 1U, (uint8_t)(APP_RPC_CMD_PING | 0x80U), ping_reply, 5U);
    host.tx_length = 0U;

    feed_frame(&host, 2U, APP_RPC_CMD_LED_SET, led_on, 1U);
    CHECK(app_rpc_poll(&rpc) == FW_OK);
    expect_frame(&host, 2U, (uint8_t)(APP_RPC_CMD_LED_SET | 0x80U), led_reply, 1U);
    CHECK(host.pin_level);
    host.tx_length = 0U;

    feed_frame(&host, 3U, APP_RPC_CMD_STATUS, NULL, 0U);
    CHECK(app_rpc_poll(&rpc) == FW_OK);
    expect_frame(&host, 3U, (uint8_t)(APP_RPC_CMD_STATUS | 0x80U), status_reply, 3U);
    host.tx_length = 0U;

    /* 0x7F 不在命令表中，回复状态码 2。 */
    feed_frame(&host, 4U, 0x7FU, NULL, 0U);
    CHECK(app_rpc_poll(&rpc) == FW_OK);
    expect_frame(&host, 4U, 0xFFU, unknown_reply, 1U);
    host.tx_length = 0U;

    /* STATS 回复 14 字节 payload；此时已处理 5 帧（含未知命令）。 */
    feed_frame(&host, 5U, APP_RPC_CMD_STATS, NULL, 0U);
    CHECK(app_rpc_poll(&rpc) == FW_OK);
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
        CHECK(stats.command == (APP_RPC_CMD_STATS | 0x80U));
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
static void test_app_rpc_report_and_timeout(void)
{
    fw_host_t host;
    app_rpc_t rpc;
    app_rpc_config_t config;
    const uint8_t report_payload[] = {0U, 0U, 0U};
    const uint8_t partial[] = {0xA5U, 0x5AU, 0x01U, 0x01U, 0x01U};

    rpc_setup(&host, &rpc, &config);
    CHECK(app_rpc_set_report_period(&rpc, 1000U) == FW_OK);
    host_advance(&host, 1000U);
    CHECK(app_rpc_poll(&rpc) == FW_OK);
    expect_frame(&host, 0U, APP_RPC_EVENT_STATUS, report_payload, 3U);
    host.tx_length = 0U;

    CHECK(app_rpc_set_report_period(&rpc, 0U) == FW_OK);
    CHECK(host_serial_feed(&host, partial, sizeof(partial)) == sizeof(partial));
    CHECK(app_rpc_poll(&rpc) == FW_OK);
    CHECK(rpc.partial_timeouts == 0U);
    host_advance(&host, 250U);
    CHECK(app_rpc_poll(&rpc) == FW_OK);
    CHECK(rpc.partial_timeouts == 1U);
}

/*
 * 背压测试。
 *
 * 把 TX ring 限制为 2 字节，发送一个 PING 请求。
 * 回复帧远大于 2 字节，因此：
 *   - poll 返回 FW_EAGAIN；
 *   - rpc.tx_offset==2，说明只推进了 2 字节；
 *   - 恢复容量后再次 poll，应把剩余部分发完并清空 tx_length。
 */
static void test_app_rpc_backpressure(void)
{
    fw_host_t host;
    app_rpc_t rpc;
    app_rpc_config_t config;

    rpc_setup(&host, &rpc, &config);

    host_serial_set_write_limit(&host, 2U);
    feed_frame(&host, 7U, APP_RPC_CMD_PING, NULL, 0U);
    CHECK(app_rpc_poll(&rpc) == FW_EAGAIN);
    CHECK(rpc.tx_offset == 2U);
    CHECK(rpc.tx_length != 0U);

    host_serial_set_write_limit(&host, HOST_SERIAL_CAPACITY);
    CHECK(app_rpc_poll(&rpc) == FW_OK);
    CHECK(rpc.tx_length == 0U);
}

static uint8_t s_task_stack[1024];

static void task_entry(void *arg)
{
    (void)arg;
}

/*
 * 初始化带任务管理器的 RPC 环境。
 *
 * 任务 0 是 adopt 的 main（不可控、无心跳），任务 1 是可控制的 worker。
 * 初始 current=main，因此 summary 的 current_index=0。
 */
static void rpc_setup_tasks(fw_host_t *host, app_rpc_t *rpc,
                            app_rpc_config_t *config, host_tasks_t *tasks)
{
    fw_task_adopt_config_t main_config;
    fw_task_create_config_t worker_config;

    host_init(host);
    host_register_devices(host);
    CHECK(host_tasks_init(tasks, host_clock(host), 1000U) == FW_OK);

    main_config.name = "main";
    main_config.health.heartbeat_enabled = false;
    main_config.health.controllable = false;
    main_config.health.heartbeat_warn_ms = 0U;
    main_config.health.heartbeat_lost_ms = 0U;
    main_config.health.stack_warn_free = 128U;
    CHECK(fw_task_adopt(&tasks->manager, &main_config) != NULL);

    worker_config.name = "worker";
    worker_config.entry = task_entry;
    worker_config.arg = NULL;
    worker_config.stack = s_task_stack;
    worker_config.stack_size = sizeof(s_task_stack);
    worker_config.priority = 9U;
    worker_config.time_slice = 20U;
    worker_config.health.heartbeat_enabled = true;
    worker_config.health.controllable = true;
    worker_config.health.heartbeat_warn_ms = 1000U;
    worker_config.health.heartbeat_lost_ms = 2000U;
    worker_config.health.stack_warn_free = 128U;
    CHECK(fw_task_create(&tasks->manager, &worker_config) != NULL);
    CHECK(fw_task_start(&tasks->manager, &tasks->manager.tasks[1]) == FW_OK);

    host_task_set_current(tasks, 0U);
    fw_task_refresh(&tasks->manager);

    config->serial = &host->serial.dev;
    config->pin = &host->pin.dev;
    config->led_pin = HOST_LED_PIN;
    config->led_active_high = true;
    config->clock = host_clock(host);
    config->tasks = &tasks->manager;
    CHECK(app_rpc_init(rpc, config) == FW_OK);
    CHECK(app_rpc_set_limits(rpc, 250U, 20U, 64U) == FW_OK);
}

/* 0x05/0x06/0x07 任务命令的帧布局与控制语义。 */
static void test_app_rpc_tasks(void)
{
    fw_host_t host;
    host_tasks_t tasks;
    app_rpc_t rpc;
    app_rpc_config_t config;
    uint8_t index;
    const uint8_t summary_expected[] = {
        APP_RPC_ST_OK, 2U, 2U, 0U, 0U, FW_TASK_NO_INDEX, 0U,
        0U, 0U, 0U, 0U, 0U, 0U
    };
    uint8_t detail_main[APP_RPC_TASK_DETAIL_LEN] = {0};
    uint8_t detail_worker[APP_RPC_TASK_DETAIL_LEN] = {0};
    const uint8_t ok_reply[] = {APP_RPC_ST_OK};
    const uint8_t not_allowed[] = {APP_RPC_ST_NOT_ALLOWED};
    const uint8_t bad_index[] = {APP_RPC_ST_BAD_INDEX};

    rpc_setup_tasks(&host, &rpc, &config, &tasks);

    feed_frame(&host, 10U, APP_RPC_CMD_TASK_SUMMARY, NULL, 0U);
    CHECK(app_rpc_poll(&rpc) == FW_OK);
    expect_frame(&host, 10U, (uint8_t)(APP_RPC_CMD_TASK_SUMMARY | 0x80U),
                 summary_expected, sizeof(summary_expected));
    host.tx_length = 0U;

    /* main: READY -> RUNNING（current），优先级 10，无心跳。 */
    detail_main[0] = APP_RPC_ST_OK;
    detail_main[1] = 0U;
    detail_main[2] = FW_TASK_STATE_RUNNING;
    detail_main[3] = FW_TASK_HEALTH_OK;
    detail_main[4] = 0U;
    detail_main[5] = 10U;
    memcpy(&detail_main[6], "main", 4U);
    detail_main[14] = 0x00U; detail_main[15] = 0x10U; /* 4096 */
    detail_main[16] = 0x00U; detail_main[17] = 0x04U; /* 1024 */
    detail_main[18] = 0x00U; detail_main[19] = 0x0CU; /* 3072 */
    detail_main[22] = 0xFFU; detail_main[23] = 0xFFU;
    detail_main[24] = 0xFFU; detail_main[25] = 0xFFU;

    index = 0U;
    feed_frame(&host, 11U, APP_RPC_CMD_TASK_DETAIL, &index, 1U);
    CHECK(app_rpc_poll(&rpc) == FW_OK);
    expect_frame(&host, 11U, (uint8_t)(APP_RPC_CMD_TASK_DETAIL | 0x80U),
                 detail_main, sizeof(detail_main));
    host.tx_length = 0U;

    /* worker: READY，优先级 9，栈 1024，心跳启用。 */
    detail_worker[0] = APP_RPC_ST_OK;
    detail_worker[1] = 1U;
    detail_worker[2] = FW_TASK_STATE_READY;
    detail_worker[3] = FW_TASK_HEALTH_OK;
    detail_worker[4] = 0U;
    detail_worker[5] = 9U;
    memcpy(&detail_worker[6], "worker", 6U);
    detail_worker[14] = 0x00U; detail_worker[15] = 0x04U; /* 1024 */
    detail_worker[18] = 0x00U; detail_worker[19] = 0x04U; /* free 1024 */
    detail_worker[30] = 1U;

    index = 1U;
    feed_frame(&host, 12U, APP_RPC_CMD_TASK_DETAIL, &index, 1U);
    CHECK(app_rpc_poll(&rpc) == FW_OK);
    expect_frame(&host, 12U, (uint8_t)(APP_RPC_CMD_TASK_DETAIL | 0x80U),
                 detail_worker, sizeof(detail_worker));
    host.tx_length = 0U;

    /* 越界索引。 */
    index = 2U;
    feed_frame(&host, 13U, APP_RPC_CMD_TASK_DETAIL, &index, 1U);
    CHECK(app_rpc_poll(&rpc) == FW_OK);
    expect_frame(&host, 13U, (uint8_t)(APP_RPC_CMD_TASK_DETAIL | 0x80U),
                 bad_index, sizeof(bad_index));
    host.tx_length = 0U;

    /* 非当前任务 suspend：只置 pending。 */
    {
        const uint8_t suspend[] = {1U, APP_RPC_TASK_SUSPEND, 0U};
        host_task_set_current(&tasks, 0U);
        feed_frame(&host, 14U, APP_RPC_CMD_TASK_CONTROL, suspend,
                   sizeof(suspend));
        CHECK(app_rpc_poll(&rpc) == FW_OK);
        expect_frame(&host, 14U, (uint8_t)(APP_RPC_CMD_TASK_CONTROL | 0x80U),
                     ok_reply, sizeof(ok_reply));
        CHECK(tasks.manager.tasks[1].suspend_pending);
        host.tx_length = 0U;
    }

    /* 目标任务在 heartbeat_self 中协作挂起。 */
    host_task_set_current(&tasks, 1U);
    fw_task_heartbeat_self(&tasks.manager);
    CHECK(tasks.tasks[1].state == FW_TASK_STATE_SUSPENDED);

    /* 未进入用户挂起，resume 拒绝。 */
    {
        const uint8_t resume[] = {1U, APP_RPC_TASK_RESUME, 0U};
        feed_frame(&host, 15U, APP_RPC_CMD_TASK_CONTROL, resume,
                   sizeof(resume));
        CHECK(app_rpc_poll(&rpc) == FW_OK);
        expect_frame(&host, 15U, (uint8_t)(APP_RPC_CMD_TASK_CONTROL | 0x80U),
                     not_allowed, sizeof(not_allowed));
        host.tx_length = 0U;
    }

    /* 当前任务不能挂起自己。 */
    {
        const uint8_t self_suspend[] = {1U, APP_RPC_TASK_SUSPEND, 0U};
        feed_frame(&host, 16U, APP_RPC_CMD_TASK_CONTROL, self_suspend,
                   sizeof(self_suspend));
        CHECK(app_rpc_poll(&rpc) == FW_OK);
        expect_frame(&host, 16U, (uint8_t)(APP_RPC_CMD_TASK_CONTROL | 0x80U),
                     not_allowed, sizeof(not_allowed));
        host.tx_length = 0U;
    }

    /* 改优先级。 */
    {
        const uint8_t set_priority[] = {1U, APP_RPC_TASK_SET_PRIORITY, 5U};
        feed_frame(&host, 17U, APP_RPC_CMD_TASK_CONTROL, set_priority,
                   sizeof(set_priority));
        CHECK(app_rpc_poll(&rpc) == FW_OK);
        expect_frame(&host, 17U, (uint8_t)(APP_RPC_CMD_TASK_CONTROL | 0x80U),
                     ok_reply, sizeof(ok_reply));
        CHECK(tasks.manager.tasks[1].priority == 5U);
        CHECK(tasks.tasks[1].priority == 5U);
        host.tx_length = 0U;
    }

    /* main 不可控。 */
    {
        const uint8_t main_suspend[] = {0U, APP_RPC_TASK_SUSPEND, 0U};
        feed_frame(&host, 18U, APP_RPC_CMD_TASK_CONTROL, main_suspend,
                   sizeof(main_suspend));
        CHECK(app_rpc_poll(&rpc) == FW_OK);
        expect_frame(&host, 18U, (uint8_t)(APP_RPC_CMD_TASK_CONTROL | 0x80U),
                     not_allowed, sizeof(not_allowed));
        host.tx_length = 0U;
    }

    /* 越界控制索引。 */
    {
        const uint8_t bad_control[] = {2U, APP_RPC_TASK_SUSPEND, 0U};
        feed_frame(&host, 19U, APP_RPC_CMD_TASK_CONTROL, bad_control,
                   sizeof(bad_control));
        CHECK(app_rpc_poll(&rpc) == FW_OK);
        expect_frame(&host, 19U, (uint8_t)(APP_RPC_CMD_TASK_CONTROL | 0x80U),
                     bad_index, sizeof(bad_index));
        host.tx_length = 0U;
    }

    host_tasks_deinit(&tasks);
}

/* 运行应用层 RPC 全部测试。 */
void test_app_rpc_run(void)
{
    test_app_rpc_basic();
    test_app_rpc_report_and_timeout();
    test_app_rpc_backpressure();
    test_app_rpc_tasks();
}
