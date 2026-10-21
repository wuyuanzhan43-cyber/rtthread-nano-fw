/*
 * host_tasks.c - fake 任务端口实现。
 *
 * 层级：src/tests/host
 * 职责：用静态数组模拟 RT-Thread 的任务对象、状态、栈和当前线程。
 * 上下文：单线程测试。
 * 依赖：host_tasks.h、string.h。
 */
#include "host_tasks.h"
#include <string.h>

#define HOST_TASK_MAX_PRIORITY 32U
#define HOST_MAIN_STACK_SIZE 4096U
#define HOST_MAIN_STACK_USED 1024U

static fw_status_t fake_create(void *ctx, const fw_task_create_config_t *config,
                               void **os_task)
{
    host_tasks_t *host = (host_tasks_t *)ctx;
    host_task_t *task;

    if (host == NULL || config == NULL || os_task == NULL ||
        host->count >= FW_TASK_MAX) return FW_EIO;

    task = &host->tasks[host->count++];
    memset(task, 0, sizeof(*task));
    task->handle = task;
    task->state = FW_TASK_STATE_INIT;
    task->priority = config->priority;
    task->stack_size = (uint32_t)config->stack_size;
    task->stack_used = 0U;
    *os_task = task->handle;
    return FW_OK;
}

static fw_status_t fake_adopt(void *ctx, const fw_task_adopt_config_t *config,
                              void **os_task)
{
    host_tasks_t *host = (host_tasks_t *)ctx;
    host_task_t *task;

    (void)config;
    if (host == NULL || os_task == NULL || host->count >= FW_TASK_MAX)
        return FW_EIO;

    task = &host->tasks[host->count++];
    memset(task, 0, sizeof(*task));
    task->handle = task;
    task->state = FW_TASK_STATE_READY;
    task->priority = 10U;
    task->stack_size = HOST_MAIN_STACK_SIZE;
    task->stack_used = HOST_MAIN_STACK_USED;
    *os_task = task->handle;
    return FW_OK;
}

static fw_status_t fake_start(void *ctx, void *os_task)
{
    host_task_t *task = (host_task_t *)os_task;
    (void)ctx;
    if (task == NULL) return FW_EINVAL;
    task->state = FW_TASK_STATE_READY;
    return FW_OK;
}

static fw_status_t fake_suspend(void *ctx, void *os_task)
{
    host_task_t *task = (host_task_t *)os_task;
    (void)ctx;
    if (task == NULL) return FW_EINVAL;
    task->state = FW_TASK_STATE_SUSPENDED;
    return FW_OK;
}

static fw_status_t fake_resume(void *ctx, void *os_task)
{
    host_task_t *task = (host_task_t *)os_task;
    (void)ctx;
    if (task == NULL) return FW_EINVAL;
    task->state = FW_TASK_STATE_READY;
    return FW_OK;
}

static fw_status_t fake_set_priority(void *ctx, void *os_task, uint8_t priority)
{
    host_task_t *task = (host_task_t *)os_task;
    (void)ctx;
    if (task == NULL) return FW_EINVAL;
    task->priority = priority;
    return FW_OK;
}

static fw_status_t fake_snapshot(void *ctx, void *os_task,
                                 fw_task_os_info_t *info)
{
    host_tasks_t *host = (host_tasks_t *)ctx;
    host_task_t *task = (host_task_t *)os_task;

    if (task == NULL || info == NULL) return FW_EINVAL;
    info->state = (host != NULL && task->handle == host->current)
                      ? FW_TASK_STATE_RUNNING : task->state;
    info->priority = task->priority;
    info->stack_size = task->stack_size;
    info->stack_used = task->stack_used;
    info->stack_free = task->stack_size >= task->stack_used
                           ? task->stack_size - task->stack_used : 0U;
    info->closed = task->closed;
    return FW_OK;
}

static void *fake_current_thread(void *ctx)
{
    host_tasks_t *host = (host_tasks_t *)ctx;
    return host != NULL ? host->current : NULL;
}

static void *fake_idle_thread(void *ctx)
{
    host_tasks_t *host = (host_tasks_t *)ctx;
    return host != NULL ? host->idle : NULL;
}

static uint16_t fake_max_priority(void *ctx)
{
    (void)ctx;
    return HOST_TASK_MAX_PRIORITY;
}

static uint32_t fake_critical_enter(void *ctx)
{
    (void)ctx;
    return 0U;
}

static void fake_critical_exit(void *ctx, uint32_t state)
{
    (void)ctx;
    (void)state;
}

static const fw_task_port_ops_t host_ops_template = {
    fake_create,
    fake_adopt,
    fake_start,
    fake_suspend,
    fake_resume,
    fake_set_priority,
    fake_snapshot,
    fake_current_thread,
    fake_idle_thread,
    fake_max_priority,
    fake_critical_enter,
    fake_critical_exit,
    NULL
};

fw_status_t host_tasks_init(host_tasks_t *host, fw_clock_t clock,
                            uint32_t refresh_ms)
{
    fw_task_manager_config_t config;

    if (host == NULL || clock.now_ms == NULL) return FW_EINVAL;

    memset(host, 0, sizeof(*host));
    host->idle = &host->idle_marker;
    host->current = NULL;
    host->ops = host_ops_template;
    host->ops.ctx = host;

    config.port = &host->ops;
    config.clock = clock;
    config.refresh_period_ms = refresh_ms;
    return fw_task_manager_init(&host->manager, &config);
}

void host_tasks_deinit(host_tasks_t *host)
{
    if (host == NULL) return;
    fw_task_manager_deinit(&host->manager);
}

void host_task_set_stack(host_tasks_t *host, size_t index, uint32_t used,
                         uint32_t size)
{
    if (host == NULL || index >= host->count) return;
    host->tasks[index].stack_size = size;
    host->tasks[index].stack_used = used > size ? size : used;
}

void host_task_set_state(host_tasks_t *host, size_t index, uint8_t state)
{
    if (host == NULL || index >= host->count) return;
    host->tasks[index].state = state;
}

void host_task_set_current(host_tasks_t *host, size_t index)
{
    if (host == NULL || index >= host->count) return;
    host->current = host->tasks[index].handle;
}

void host_task_set_current_idle(host_tasks_t *host)
{
    if (host == NULL) return;
    host->current = host->idle;
}

void host_task_set_current_none(host_tasks_t *host)
{
    if (host == NULL) return;
    host->current = NULL;
}

void host_task_cpu_tick(host_tasks_t *host)
{
    (void)host;
    fw_task_cpu_tick();
}
