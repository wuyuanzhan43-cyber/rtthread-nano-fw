/*
 * test_task.c - framework 任务管理与健康度 Host 测试。
 *
 * 层级：src/tests/host
 * 职责：
 *   1. 验证心跳 OK/WARN/LOST、栈低告警和恢复；
 *   2. 验证 CPU busy/idle/other 窗口；
 *   3. 验证时间回绕、协作挂起、resume、改优先级和拒绝路径；
 *   4. 不依赖 RTOS，使用 host_tasks 的 fake port。
 *
 * 上下文：单线程。
 * 依赖：test_common.h、host_tasks.h、fw/task.h。
 */
#include "test_common.h"
#include "host_tasks.h"
#include "fw/task.h"

static uint8_t s_stack1[1024];
static uint8_t s_stack2[1024];

/* fake port 不会真正运行入口，但框架要求 entry 非空。 */
static void dummy_entry(void *arg)
{
    (void)arg;
}

static fw_task_health_config_t health_default(void)
{
    fw_task_health_config_t health;
    health.heartbeat_enabled = true;
    health.controllable = true;
    health.heartbeat_warn_ms = 1000U;
    health.heartbeat_lost_ms = 2000U;
    health.stack_warn_free = 128U;
    return health;
}

static fw_task_create_config_t create_config(const char *name, void *stack,
                                             uint8_t priority,
                                             fw_task_health_config_t health)
{
    fw_task_create_config_t config;
    config.name = name;
    config.entry = dummy_entry;
    config.arg = NULL;
    config.stack = stack;
    config.stack_size = 1024U;
    config.priority = priority;
    config.time_slice = 20U;
    config.health = health;
    return config;
}

static void setup_tasks(host_tasks_t *tasks, fw_host_t *host)
{
    fw_task_adopt_config_t main_config;
    fw_task_health_config_t health = health_default();
    fw_task_create_config_t config;

    host_init(host);
    CHECK(host_tasks_init(tasks, host_clock(host), 1000U) == FW_OK);

    main_config.name = "main";
    main_config.health = health;
    main_config.health.heartbeat_enabled = false;
    main_config.health.controllable = false;
    CHECK(fw_task_adopt(&tasks->manager, &main_config) != NULL);

    config = create_config("worker1", s_stack1, 9U, health);
    CHECK(fw_task_create(&tasks->manager, &config) != NULL);
    config = create_config("worker2", s_stack2, 8U, health);
    CHECK(fw_task_create(&tasks->manager, &config) != NULL);

    CHECK(fw_task_start(&tasks->manager, &tasks->manager.tasks[1]) == FW_OK);
    CHECK(fw_task_start(&tasks->manager, &tasks->manager.tasks[2]) == FW_OK);

    /* 建立初始心跳。 */
    host_task_set_current(tasks, 1U);
    fw_task_heartbeat_self(&tasks->manager);
    host_task_set_current(tasks, 2U);
    fw_task_heartbeat_self(&tasks->manager);
    host_task_set_current(tasks, 0U);
}

static void test_task_health(void)
{
    host_tasks_t tasks;
    fw_host_t host;
    fw_task_summary_t summary;
    fw_task_info_t info;

    setup_tasks(&tasks, &host);

    fw_task_refresh(&tasks.manager);
    CHECK(fw_task_summary(&tasks.manager, &summary) == FW_OK);
    CHECK(summary.total == 3U);
    CHECK(summary.ok == 3U);
    CHECK(summary.warn == 0U);
    CHECK(summary.lost == 0U);
    CHECK(summary.worst_index == FW_TASK_NO_INDEX);
    CHECK(summary.current_index == 0U);
    CHECK(fw_task_get_info(&tasks.manager, 1U, &info) == FW_OK);
    CHECK(info.health == FW_TASK_HEALTH_OK);
    CHECK(info.heartbeat_enabled);
    CHECK(info.heartbeat_age_ms < 10U);

    /* 栈剩余 24 字节，低于 128 字节阈值。 */
    host_task_set_stack(&tasks, 1U, 1000U, 1024U);
    fw_task_refresh(&tasks.manager);
    CHECK(fw_task_summary(&tasks.manager, &summary) == FW_OK);
    CHECK(summary.ok == 2U && summary.warn == 1U);
    CHECK(summary.worst_index == 1U);
    CHECK(fw_task_get_info(&tasks.manager, 1U, &info) == FW_OK);
    CHECK((info.reasons & FW_TASK_REASON_STACK_LOW) != 0U);

    /* 心跳 warn。 */
    host_advance(&host, 1200U);
    fw_task_refresh(&tasks.manager);
    CHECK(fw_task_summary(&tasks.manager, &summary) == FW_OK);
    CHECK(summary.warn == 2U && summary.ok == 1U);
    CHECK(fw_task_get_info(&tasks.manager, 2U, &info) == FW_OK);
    CHECK((info.reasons & FW_TASK_REASON_HB_WARN) != 0U);

    /* 心跳 lost。 */
    host_advance(&host, 900U);
    fw_task_refresh(&tasks.manager);
    CHECK(fw_task_summary(&tasks.manager, &summary) == FW_OK);
    CHECK(summary.lost == 2U);

    /* worker2 恢复心跳，worker1 仍 lost。 */
    host_task_set_current(&tasks, 2U);
    fw_task_heartbeat_self(&tasks.manager);
    host_task_set_current(&tasks, 0U);
    fw_task_refresh(&tasks.manager);
    CHECK(fw_task_summary(&tasks.manager, &summary) == FW_OK);
    CHECK(summary.lost == 1U && summary.ok == 2U);
    CHECK(fw_task_get_info(&tasks.manager, 2U, &info) == FW_OK);
    CHECK(info.health == FW_TASK_HEALTH_OK);
    CHECK(info.heartbeat_count >= 2U);

    /* 心跳的 warn/lost 计时不能导致 next_wait 永久为 0。 */
    CHECK(fw_task_next_wait_ms(&tasks.manager) > 0U);

    host_tasks_deinit(&tasks);
}

static void test_task_cpu(void)
{
    host_tasks_t tasks;
    fw_host_t host;
    fw_task_summary_t summary;
    fw_task_info_t info;
    unsigned i;

    setup_tasks(&tasks, &host);

    for (i = 0U; i < 100U; ++i) {
        if (i < 30U) {
            host_task_set_current_idle(&tasks);
        } else if (i < 50U) {
            host_task_set_current(&tasks, 1U);
        } else if (i < 60U) {
            host_task_set_current(&tasks, 2U);
        } else {
            host_task_set_current_none(&tasks);
        }
        host_task_cpu_tick(&tasks);
    }

    fw_task_refresh(&tasks.manager);
    CHECK(fw_task_summary(&tasks.manager, &summary) == FW_OK);
    CHECK(summary.idle_permille == 300U);
    CHECK(summary.other_permille == 400U);
    CHECK(summary.busy_permille == 300U);
    CHECK(fw_task_get_info(&tasks.manager, 1U, &info) == FW_OK);
    CHECK(info.cpu_permille == 200U);
    CHECK(fw_task_get_info(&tasks.manager, 2U, &info) == FW_OK);
    CHECK(info.cpu_permille == 100U);

    host_tasks_deinit(&tasks);
}

static void test_task_control(void)
{
    host_tasks_t tasks;
    fw_host_t host;

    setup_tasks(&tasks, &host);

    /* main 不可控。 */
    CHECK(fw_task_control(&tasks.manager, 0U, FW_TASK_CTRL_SUSPEND, 0U)
          == FW_ENOTSUP);

    /* 非当前任务：只置 pending。 */
    host_task_set_current(&tasks, 0U);
    CHECK(fw_task_control(&tasks.manager, 1U, FW_TASK_CTRL_SUSPEND, 0U)
          == FW_OK);
    CHECK(tasks.manager.tasks[1].suspend_pending);

    /* 目标任务在 heartbeat_self 中协作挂起。 */
    host_task_set_current(&tasks, 1U);
    fw_task_heartbeat_self(&tasks.manager);
    CHECK(!tasks.manager.tasks[1].suspend_pending);
    CHECK(tasks.tasks[1].state == FW_TASK_STATE_SUSPENDED);

    /* 没有 user_suspended 标记时不能恢复。 */
    CHECK(fw_task_control(&tasks.manager, 1U, FW_TASK_CTRL_RESUME, 0U)
          == FW_EBUSY);

    /* 模拟 port 已经进入用户挂起；resume 应清除标记并恢复 READY。 */
    tasks.manager.tasks[1].user_suspended = true;
    tasks.tasks[1].state = FW_TASK_STATE_SUSPENDED;
    CHECK(fw_task_control(&tasks.manager, 1U, FW_TASK_CTRL_RESUME, 0U)
          == FW_OK);
    CHECK(!tasks.manager.tasks[1].user_suspended);
    CHECK(tasks.tasks[1].state == FW_TASK_STATE_READY);

    /* 改优先级。 */
    CHECK(fw_task_control(&tasks.manager, 1U, FW_TASK_CTRL_SET_PRIORITY, 5U)
          == FW_OK);
    CHECK(tasks.manager.tasks[1].priority == 5U);
    CHECK(tasks.tasks[1].priority == 5U);

    /* 当前任务不能挂起自己。 */
    host_task_set_current(&tasks, 2U);
    CHECK(fw_task_control(&tasks.manager, 2U, FW_TASK_CTRL_SUSPEND, 0U)
          == FW_EBUSY);

    CHECK(fw_task_control(&tasks.manager, FW_TASK_MAX, FW_TASK_CTRL_SUSPEND, 0U)
          == FW_EINVAL);

    host_tasks_deinit(&tasks);
}

static void test_task_time_wrap(void)
{
    host_tasks_t tasks;
    fw_host_t host;
    fw_task_info_t info;

    setup_tasks(&tasks, &host);

    /* 把时钟放到回绕点前 256ms，再推进 512ms 跨过 2^32。 */
    host.now_ms = UINT32_MAX - 255U;
    host_task_set_current(&tasks, 2U);
    fw_task_heartbeat_self(&tasks.manager);
    host_advance(&host, 512U);
    fw_task_refresh(&tasks.manager);

    CHECK(fw_task_get_info(&tasks.manager, 2U, &info) == FW_OK);
    CHECK(info.heartbeat_age_ms == 512U);
    CHECK(info.health == FW_TASK_HEALTH_OK);

    host_tasks_deinit(&tasks);
}

static void test_task_next_wait(void)
{
    host_tasks_t tasks;
    fw_host_t host;

    setup_tasks(&tasks, &host);
    fw_task_refresh(&tasks.manager);
    CHECK(fw_task_next_wait_ms(&tasks.manager) <= 1000U);

    /* 900ms 后，warn 剩余 100ms，重新刷新也应被安排。 */
    host_advance(&host, 900U);
    CHECK(fw_task_next_wait_ms(&tasks.manager) <= 100U);

    host_tasks_deinit(&tasks);
}

void test_task_run(void)
{
    test_task_health();
    test_task_cpu();
    test_task_control();
    test_task_time_wrap();
    test_task_next_wait();
}
