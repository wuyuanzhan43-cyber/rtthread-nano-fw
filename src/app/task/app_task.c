/*
 * app_task.c - 应用任务装配：main 监督者 + app_rpc 工作线程。
 *
 * 层级：src/app/task
 * 职责：
 *   1. 静态 manager、app_rpc 实例和 RPC 栈；
 *   2. adopt RT-Thread main，创建/启动 RPC 任务；
 *   3. RPC 任务入口：心跳、poll、wait_rx。
 *
 * 上下文：app_tasks_init/start 在 main 线程；入口在 RPC 任务。
 * 依赖：app/tasks.h、app_config.h、fw/rtthread_task.h、
 *       fw/rtthread_clock.h。
 *
 * 设计说明：
 *   - RPC 栈用 uint64_t 数组保证 8 字节对齐；
 *   - 等待上限 clamp 到 APP_RPC_LOOP_WAIT_MAX_MS，保证心跳年龄
 *     不会因为周期上报的长等待而超过 warn 阈值；
 *   - app_rpc_t 放在静态区，main 栈只保留控制流。
 */
#include "app/tasks.h"
#include "app_config.h"
#include "fw/rtthread_task.h"
#include "fw/rtthread_clock.h"
#include <stdint.h>

#if (APP_RPC_TASK_STACK_SIZE % 8) != 0
#error "APP_RPC_TASK_STACK_SIZE must be a multiple of 8"
#endif

/* RPC 任务的静态栈；uint64_t 数组天然满足 8 字节对齐。 */
static uint64_t s_rpc_stack[APP_RPC_TASK_STACK_SIZE / 8];

/* 应用静态对象；生命周期覆盖整个程序。 */
static app_rpc_t s_rpc;
static fw_task_manager_t s_tasks;
static fw_task_t *s_rpc_task;
static bool s_initialized;

/*
 * RPC 任务入口。
 *
 * 每轮先更新心跳（同时处理 pending 挂起），再处理 RX/TX；
 * 等待上限为 APP_RPC_LOOP_WAIT_MAX_MS，避免心跳饥饿。
 */
static void app_rpc_task_entry(void *arg)
{
    app_rpc_t *rpc = (app_rpc_t *)arg;
    uint32_t wait;

    for (;;) {
        fw_task_heartbeat_self(&s_tasks);

        (void)app_rpc_poll(rpc);

        wait = app_rpc_next_wait_ms(rpc);
        if (wait == APP_RPC_WAIT_FOREVER || wait > APP_RPC_LOOP_WAIT_MAX_MS)
            wait = APP_RPC_LOOP_WAIT_MAX_MS;

        (void)fw_serial_wait_rx(rpc->serial, wait);
    }
}

fw_status_t app_tasks_init(void)
{
    fw_task_manager_config_t manager_config;
    fw_task_adopt_config_t main_config;
    fw_task_create_config_t rpc_config;
    fw_status_t status;

    if (s_initialized) return FW_OK;

    manager_config.port = fw_rtthread_task_port();
    manager_config.clock = fw_rtthread_clock();
    manager_config.refresh_period_ms = APP_HEALTH_REFRESH_MS;

    status = fw_task_manager_init(&s_tasks, &manager_config);
    if (status != FW_OK) return status;

    main_config.name = "main";
    main_config.health.heartbeat_enabled = false;
    main_config.health.controllable = false;
    main_config.health.heartbeat_warn_ms = 0U;
    main_config.health.heartbeat_lost_ms = 0U;
    main_config.health.stack_warn_free = APP_TASK_STACK_WARN_BYTES;

    if (fw_task_adopt(&s_tasks, &main_config) == NULL) {
        fw_task_manager_deinit(&s_tasks);
        return FW_EIO;
    }

    rpc_config.name = "rpc";
    rpc_config.entry = app_rpc_task_entry;
    rpc_config.arg = &s_rpc;
    rpc_config.stack = s_rpc_stack;
    rpc_config.stack_size = sizeof(s_rpc_stack);
    rpc_config.priority = APP_RPC_TASK_PRIORITY;
    rpc_config.time_slice = 20U;
    rpc_config.health.heartbeat_enabled = true;
    rpc_config.health.controllable = true;
    rpc_config.health.heartbeat_warn_ms = APP_TASK_HEARTBEAT_WARN_MS;
    rpc_config.health.heartbeat_lost_ms = APP_TASK_HEARTBEAT_LOST_MS;
    rpc_config.health.stack_warn_free = APP_TASK_STACK_WARN_BYTES;

    s_rpc_task = fw_task_create(&s_tasks, &rpc_config);
    if (s_rpc_task == NULL) {
        fw_task_manager_deinit(&s_tasks);
        return FW_EIO;
    }

    s_initialized = true;
    return FW_OK;
}

fw_status_t app_tasks_start(void)
{
    if (!s_initialized || s_rpc_task == NULL) return FW_ENOTREADY;
    return fw_task_start(&s_tasks, s_rpc_task);
}

fw_task_manager_t *app_tasks_manager(void)
{
    return &s_tasks;
}

app_rpc_t *app_tasks_rpc(void)
{
    return &s_rpc;
}
