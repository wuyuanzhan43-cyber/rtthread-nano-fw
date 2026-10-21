/*
 * fw_task.c - 任务注册表、健康度计算与运行时控制。
 *
 * 层级：src/framework/components/task
 * 职责：
 *   1. 静态任务表与 create/adopt/start 流程；
 *   2. 心跳、栈高水位、状态与 CPU 窗口的健康计算；
 *   3. summary/detail/control 的框架侧实现；
 *   4. SysTick 中断中的 CPU tick 归桶。
 *
 * 上下文：
 *   - cpu_tick：SysTick 中断，只做指针比较和 volatile 自增；
 *   - refresh：监督任务，栈扫描在临界区外；
 *   - summary/get_info/control：RPC 任务，短临界区复制/修改字段。
 *
 * 依赖：fw/task.h、string.h。
 *
 * 设计说明：
 *   - 本文件不包含 rtthread.h；线程操作全部通过 port ops；
 *   - 单实例：cpu_tick 使用模块内全局 manager 指针；
 *   - 不使用 malloc，任务表、统计和快照数组都在静态存储中。
 */
#include "fw/task.h"
#include <string.h>

/* cpu_tick 使用的唯一 manager；未初始化时为 NULL。 */
static fw_task_manager_t *g_task_manager;

/* 临界区进出；只用于短小的字段复制和计数器读取。 */
static uint32_t critical_enter(fw_task_manager_t *manager)
{
    return manager->port->critical_enter(manager->port->ctx);
}

static void critical_exit(fw_task_manager_t *manager, uint32_t state)
{
    manager->port->critical_exit(manager->port->ctx, state);
}

/* 把 name 复制到固定长度字段，超长截断并保证 NUL 结尾。 */
static void copy_name(char *dst, const char *src)
{
    size_t i = 0U;

    if (src != NULL) {
        while (i < (FW_TASK_NAME_MAX - 1U) && src[i] != '\0') {
            dst[i] = src[i];
            ++i;
        }
    }
    dst[i] = '\0';
}

/* 按 os_task 句柄查找托管任务；未找到返回 NULL。 */
static fw_task_t *find_by_os_task(fw_task_manager_t *manager, void *os_task)
{
    size_t i;

    if (os_task == NULL) return NULL;
    for (i = 0U; i < manager->count; ++i) {
        if (manager->tasks[i].created && manager->tasks[i].os_task == os_task)
            return &manager->tasks[i];
    }
    return NULL;
}

/* 当前线程对应的托管任务；未托管返回 NULL。 */
static fw_task_t *find_current_task(fw_task_manager_t *manager)
{
    void *current = manager->port->current_thread(manager->port->ctx);
    return find_by_os_task(manager, current);
}

/* 读取当前毫秒；manager/clock 为空时返回 0。 */
static uint32_t manager_now_ms(const fw_task_manager_t *manager)
{
    return fw_clock_now_ms(&manager->clock);
}

/* 更新一次心跳；由 heartbeat 与 heartbeat_self 共用。 */
static void update_heartbeat(fw_task_t *task, uint32_t now)
{
    uint32_t state;

    if (task == NULL || task->manager == NULL ||
        !task->manager->initialized) return;

    state = critical_enter(task->manager);
    task->last_heartbeat_ms = now;
    ++task->heartbeat_count;
    critical_exit(task->manager, state);
}

fw_status_t fw_task_manager_init(fw_task_manager_t *manager,
                                 const fw_task_manager_config_t *config)
{
    if (manager == NULL || config == NULL || config->port == NULL ||
        config->clock.now_ms == NULL) return FW_EINVAL;
    if (config->port->create == NULL || config->port->adopt == NULL ||
        config->port->start == NULL || config->port->suspend == NULL ||
        config->port->resume == NULL || config->port->set_priority == NULL ||
        config->port->snapshot == NULL || config->port->current_thread == NULL ||
        config->port->idle_thread == NULL || config->port->max_priority == NULL ||
        config->port->critical_enter == NULL ||
        config->port->critical_exit == NULL) return FW_EINVAL;

    memset(manager, 0, sizeof(*manager));
    manager->port = config->port;
    manager->clock = config->clock;
    manager->refresh_period_ms = config->refresh_period_ms != 0U
                                     ? config->refresh_period_ms : 1000U;
    manager->priority_max = config->port->max_priority(config->port->ctx);
    if (manager->priority_max == 0U) return FW_EINVAL;

    manager->last_refresh_ms = manager_now_ms(manager);
    manager->worst_index = FW_TASK_NO_INDEX;
    manager->initialized = true;
    g_task_manager = manager;
    return FW_OK;
}

void fw_task_manager_deinit(fw_task_manager_t *manager)
{
    if (manager == NULL) return;
    if (g_task_manager == manager) g_task_manager = NULL;
    manager->count = 0U;
    manager->initialized = false;
}

fw_task_t *fw_task_create(fw_task_manager_t *manager,
                          const fw_task_create_config_t *config)
{
    fw_task_t *task;
    uint32_t now;

    if (manager == NULL || !manager->initialized || config == NULL ||
        config->name == NULL || config->entry == NULL ||
        config->stack == NULL || config->stack_size < 256U ||
        config->priority >= manager->priority_max ||
        manager->count >= FW_TASK_MAX) return NULL;

    task = &manager->tasks[manager->count];
    memset(task, 0, sizeof(*task));
    task->manager = manager;
    copy_name(task->name, config->name);
    task->entry = config->entry;
    task->arg = config->arg;
    task->stack = config->stack;
    task->stack_size = config->stack_size;
    task->priority = config->priority;
    task->time_slice = config->time_slice;
    task->heartbeat_enabled = config->health.heartbeat_enabled;
    task->controllable = config->health.controllable;
    task->heartbeat_warn_ms = config->health.heartbeat_warn_ms;
    task->heartbeat_lost_ms = config->health.heartbeat_lost_ms;
    task->stack_warn_free = config->health.stack_warn_free;
    task->state = FW_TASK_STATE_INIT;
    task->health = FW_TASK_HEALTH_OK;

    if (manager->port->create(manager->port->ctx, config,
                              &task->os_task) != FW_OK) {
        memset(task, 0, sizeof(*task));
        return NULL;
    }

    now = manager_now_ms(manager);
    task->last_heartbeat_ms = now;
    task->created = true;
    ++manager->count;
    return task;
}

fw_task_t *fw_task_adopt(fw_task_manager_t *manager,
                         const fw_task_adopt_config_t *config)
{
    fw_task_t *task;
    fw_task_os_info_t os;
    uint32_t now;

    if (manager == NULL || !manager->initialized || config == NULL ||
        config->name == NULL || manager->count >= FW_TASK_MAX) return NULL;

    task = &manager->tasks[manager->count];
    memset(task, 0, sizeof(*task));
    task->manager = manager;
    copy_name(task->name, config->name);
    task->heartbeat_enabled = config->health.heartbeat_enabled;
    task->controllable = config->health.controllable;
    task->heartbeat_warn_ms = config->health.heartbeat_warn_ms;
    task->heartbeat_lost_ms = config->health.heartbeat_lost_ms;
    task->stack_warn_free = config->health.stack_warn_free;

    if (manager->port->adopt(manager->port->ctx, config,
                             &task->os_task) != FW_OK) {
        memset(task, 0, sizeof(*task));
        return NULL;
    }

    (void)manager->port->snapshot(manager->port->ctx, task->os_task, &os);
    task->priority = os.priority;
    task->stack_size = os.stack_size;
    task->stack_used = os.stack_used;
    task->stack_free = os.stack_free;
    task->state = os.state;
    task->closed = os.closed;
    task->health = FW_TASK_HEALTH_OK;

    now = manager_now_ms(manager);
    task->last_heartbeat_ms = now;
    task->created = true;
    task->started = true;
    ++manager->count;
    return task;
}

fw_status_t fw_task_start(fw_task_manager_t *manager, fw_task_t *task)
{
    uint32_t now;

    if (manager == NULL || !manager->initialized || task == NULL ||
        task->manager != manager || !task->created || task->started)
        return FW_EINVAL;

    if (manager->port->start(manager->port->ctx, task->os_task) != FW_OK)
        return FW_EIO;

    now = manager_now_ms(manager);
    task->started = true;
    task->last_heartbeat_ms = now;
    return FW_OK;
}

void fw_task_heartbeat(fw_task_t *task)
{
    if (task == NULL || task->manager == NULL ||
        !task->manager->initialized) return;
    update_heartbeat(task, manager_now_ms(task->manager));
}

void fw_task_heartbeat_self(fw_task_manager_t *manager)
{
    fw_task_t *task;
    uint32_t now;
    uint32_t state;
    bool pending;

    if (manager == NULL || !manager->initialized) return;
    task = find_current_task(manager);
    if (task == NULL) return;

    state = critical_enter(manager);
    pending = task->suspend_pending;
    if (pending) task->suspend_pending = false;
    critical_exit(manager, state);

    now = manager_now_ms(manager);
    if (task->heartbeat_enabled) update_heartbeat(task, now);

    if (pending) {
        /* 标记为用户挂起，健康检查跳过心跳超时。 */
        state = critical_enter(manager);
        task->user_suspended = true;
        critical_exit(manager, state);

        if (manager->port->suspend(manager->port->ctx, task->os_task) != FW_OK) {
            state = critical_enter(manager);
            task->user_suspended = false;
            critical_exit(manager, state);
        } else {
            /* 被 resume 后从这里继续；control 已经清了标志。 */
            state = critical_enter(manager);
            task->user_suspended = false;
            critical_exit(manager, state);
            now = manager_now_ms(manager);
            if (task->heartbeat_enabled) update_heartbeat(task, now);
        }
    }
}

void fw_task_refresh(fw_task_manager_t *manager)
{
    fw_task_os_info_t os[FW_TASK_MAX];
    uint32_t last_heartbeat[FW_TASK_MAX];
    uint32_t cpu_snapshot[FW_TASK_MAX];
    uint32_t total_ticks;
    uint32_t idle_ticks;
    uint32_t other_ticks;
    uint32_t now;
    uint32_t total_delta;
    uint32_t idle_delta;
    uint32_t other_delta;
    uint32_t state;
    size_t i;

    if (manager == NULL || !manager->initialized || manager->clock.now_ms == NULL)
        return;

    /* 栈扫描可能较慢，放在临界区外；单核下其他任务此时不在运行。 */
    for (i = 0U; i < manager->count; ++i) {
        memset(&os[i], 0, sizeof(os[i]));
        if (manager->tasks[i].created && manager->tasks[i].os_task != NULL) {
            (void)manager->port->snapshot(manager->port->ctx,
                                          manager->tasks[i].os_task, &os[i]);
        }
    }

    /* 心跳与 CPU 计数器在同一个关中断窗口内取快照。 */
    state = critical_enter(manager);
    now = manager_now_ms(manager);
    total_ticks = manager->cpu_total_ticks;
    idle_ticks = manager->cpu_idle_ticks;
    other_ticks = manager->cpu_other_ticks;
    for (i = 0U; i < manager->count; ++i) {
        last_heartbeat[i] = manager->tasks[i].last_heartbeat_ms;
        cpu_snapshot[i] = manager->tasks[i].cpu_ticks;
    }
    critical_exit(manager, state);

    total_delta = total_ticks - manager->last_total_ticks;
    idle_delta = idle_ticks - manager->last_idle_ticks;
    other_delta = other_ticks - manager->last_other_ticks;
    manager->last_total_ticks = total_ticks;
    manager->last_idle_ticks = idle_ticks;
    manager->last_other_ticks = other_ticks;

    for (i = 0U; i < manager->count; ++i) {
        fw_task_t *task = &manager->tasks[i];
        uint8_t health = FW_TASK_HEALTH_OK;
        uint8_t reasons = 0U;
        uint32_t delta;
        uint16_t permille = 0U;
        bool t_started;
        bool t_heartbeat;
        bool t_user_suspended;
        bool t_closed;
        uint32_t t_warn;
        uint32_t t_lost;
        uint32_t t_stack_warn;

        if (!task->created) continue;

        /* 读取易变策略字段；写入统一放在本任务末尾的临界区。 */
        state = critical_enter(manager);
        t_started = task->started;
        t_heartbeat = task->heartbeat_enabled;
        t_user_suspended = task->user_suspended;
        t_closed = task->closed || os[i].closed;
        t_warn = task->heartbeat_warn_ms;
        t_lost = task->heartbeat_lost_ms;
        t_stack_warn = task->stack_warn_free;
        delta = cpu_snapshot[i] - task->last_cpu_ticks;
        task->last_cpu_ticks = cpu_snapshot[i];
        critical_exit(manager, state);

        if (total_delta != 0U) {
            permille = (uint16_t)((delta * 1000U) / total_delta);
            if (permille > 1000U) permille = 1000U;
        }

        if (t_closed) {
            health = FW_TASK_HEALTH_LOST;
            reasons |= FW_TASK_REASON_CLOSED;
        } else {
            if (t_stack_warn != 0U &&
                os[i].stack_free <= t_stack_warn) {
                health = FW_TASK_HEALTH_WARN;
                reasons |= FW_TASK_REASON_STACK_LOW;
            }
            if (t_heartbeat && t_started && !t_user_suspended) {
                uint32_t age = fw_elapsed_ms(now, last_heartbeat[i]);
                if (t_lost != 0U && age >= t_lost) {
                    health = FW_TASK_HEALTH_LOST;
                    reasons |= FW_TASK_REASON_HB_LOST;
                } else if (t_warn != 0U && age >= t_warn) {
                    if (health < FW_TASK_HEALTH_WARN)
                        health = FW_TASK_HEALTH_WARN;
                    reasons |= FW_TASK_REASON_HB_WARN;
                }
            }
        }

        state = critical_enter(manager);
        task->state = os[i].state;
        task->priority = os[i].priority;
        task->stack_size = os[i].stack_size;
        task->stack_used = os[i].stack_used;
        task->stack_free = os[i].stack_free;
        task->closed = t_closed;
        task->health = health;
        task->reasons = reasons;
        task->cpu_permille = permille;
        critical_exit(manager, state);
    }

    if (total_delta != 0U) {
        uint32_t busy;

        manager->idle_permille = (uint16_t)((idle_delta * 1000U) / total_delta);
        manager->other_permille = (uint16_t)((other_delta * 1000U) / total_delta);
        if (manager->idle_permille > 1000U) manager->idle_permille = 1000U;
        if (manager->other_permille > 1000U) manager->other_permille = 1000U;
        busy = 1000U;
        if (busy > manager->idle_permille) busy -= manager->idle_permille;
        else busy = 0U;
        if (busy > manager->other_permille) busy -= manager->other_permille;
        else busy = 0U;
        manager->busy_permille = (uint16_t)busy;
    } else {
        manager->idle_permille = 0U;
        manager->other_permille = 0U;
        manager->busy_permille = 0U;
    }

    manager->last_refresh_ms = now;
}

uint32_t fw_task_next_wait_ms(const fw_task_manager_t *manager)
{
    uint32_t wait;
    uint32_t elapsed;
    uint32_t now;
    uint32_t state;
    uint32_t last_heartbeat[FW_TASK_MAX];
    bool check[FW_TASK_MAX];
    uint32_t warn[FW_TASK_MAX];
    uint32_t lost[FW_TASK_MAX];
    size_t count;
    size_t i;

    if (manager == NULL || !manager->initialized || manager->clock.now_ms == NULL)
        return 1000U;

    now = manager_now_ms(manager);
    elapsed = fw_elapsed_ms(now, manager->last_refresh_ms);
    wait = elapsed >= manager->refresh_period_ms
               ? 0U : manager->refresh_period_ms - elapsed;

    state = critical_enter((fw_task_manager_t *)manager);
    count = manager->count;
    for (i = 0U; i < count; ++i) {
        const fw_task_t *task = &manager->tasks[i];
        last_heartbeat[i] = task->last_heartbeat_ms;
        warn[i] = task->heartbeat_warn_ms;
        lost[i] = task->heartbeat_lost_ms;
        check[i] = task->created && task->started &&
                   task->heartbeat_enabled && !task->user_suspended &&
                   !task->closed;
    }
    critical_exit((fw_task_manager_t *)manager, state);

    for (i = 0U; i < count; ++i) {
        uint32_t age;
        uint32_t remaining;

        if (!check[i]) continue;
        age = fw_elapsed_ms(now, last_heartbeat[i]);

        /* 已经超过 lost 的任务保持 LOST，不再次触发立即刷新，避免忙循环。 */
        if (lost[i] != 0U && age >= lost[i]) continue;

        if (warn[i] != 0U && age < warn[i]) {
            remaining = warn[i] - age;
            if (remaining < wait) wait = remaining;
        }
        if (lost[i] != 0U) {
            remaining = lost[i] - age;
            if (remaining < wait) wait = remaining;
        }
    }
    return wait;
}

fw_status_t fw_task_summary(const fw_task_manager_t *manager,
                            fw_task_summary_t *summary)
{
    uint32_t state;
    size_t i;
    uint8_t worst = FW_TASK_HEALTH_OK;
    uint8_t worst_index = FW_TASK_NO_INDEX;
    uint8_t current_index = FW_TASK_NO_INDEX;
    void *current;

    if (manager == NULL || !manager->initialized || summary == NULL)
        return FW_EINVAL;

    memset(summary, 0, sizeof(*summary));
    state = critical_enter((fw_task_manager_t *)manager);
    summary->total = (uint8_t)manager->count;
    current = manager->port->current_thread(manager->port->ctx);
    summary->busy_permille = manager->busy_permille;
    summary->idle_permille = manager->idle_permille;
    summary->other_permille = manager->other_permille;

    for (i = 0U; i < manager->count; ++i) {
        const fw_task_t *task = &manager->tasks[i];
        if (!task->created) continue;
        if (task->health == FW_TASK_HEALTH_LOST) {
            ++summary->lost;
            if (worst < FW_TASK_HEALTH_LOST) {
                worst = FW_TASK_HEALTH_LOST;
                worst_index = (uint8_t)i;
            }
        } else if (task->health == FW_TASK_HEALTH_WARN) {
            ++summary->warn;
            if (worst < FW_TASK_HEALTH_WARN) {
                worst = FW_TASK_HEALTH_WARN;
                worst_index = (uint8_t)i;
            }
        } else {
            ++summary->ok;
        }
        if (task->os_task == current) current_index = (uint8_t)i;
    }
    critical_exit((fw_task_manager_t *)manager, state);

    summary->worst_index = worst_index;
    summary->current_index = current_index;
    return FW_OK;
}

fw_status_t fw_task_get_info(const fw_task_manager_t *manager, size_t index,
                             fw_task_info_t *info)
{
    const fw_task_t *task;
    uint32_t last_heartbeat;
    bool heartbeat_enabled;
    bool started;
    uint32_t state;
    uint32_t now;
    void *current;

    if (manager == NULL || !manager->initialized || info == NULL ||
        index >= manager->count) return FW_EINVAL;

    task = &manager->tasks[index];
    state = critical_enter((fw_task_manager_t *)manager);
    current = manager->port->current_thread(manager->port->ctx);
    memcpy(info->name, task->name, FW_TASK_NAME_MAX);
    info->index = (uint8_t)index;
    info->state = task->os_task == current
                      ? FW_TASK_STATE_RUNNING : task->state;
    info->health = task->health;
    info->reasons = task->reasons;
    info->priority = task->priority;
    info->heartbeat_enabled = task->heartbeat_enabled;
    info->cpu_permille = task->cpu_permille;
    info->stack_size = task->stack_size;
    info->stack_used = task->stack_used;
    info->stack_free = task->stack_free;
    info->heartbeat_count = task->heartbeat_count;
    last_heartbeat = task->last_heartbeat_ms;
    heartbeat_enabled = task->heartbeat_enabled;
    started = task->started;
    critical_exit((fw_task_manager_t *)manager, state);

    if (heartbeat_enabled && started) {
        now = manager_now_ms(manager);
        info->heartbeat_age_ms = fw_elapsed_ms(now, last_heartbeat);
    } else {
        info->heartbeat_age_ms = UINT32_MAX;
    }
    return FW_OK;
}

fw_status_t fw_task_control(fw_task_manager_t *manager, size_t index,
                            fw_task_ctrl_t action, uint8_t value)
{
    fw_task_t *task;
    uint32_t state;
    void *current;
    bool controllable;
    bool started;
    bool user_suspended;
    bool closed;

    if (manager == NULL || !manager->initialized || index >= manager->count)
        return FW_EINVAL;

    task = &manager->tasks[index];
    state = critical_enter(manager);
    controllable = task->controllable;
    started = task->started;
    user_suspended = task->user_suspended;
    closed = task->closed;
    critical_exit(manager, state);

    if (!task->created || closed) return FW_ENOTREADY;

    switch (action) {
    case FW_TASK_CTRL_SUSPEND:
        if (!controllable) return FW_ENOTSUP;
        if (!started) return FW_ENOTREADY;
        current = manager->port->current_thread(manager->port->ctx);
        if (task->os_task == current) return FW_EBUSY;
        state = critical_enter(manager);
        if (!task->user_suspended) task->suspend_pending = true;
        critical_exit(manager, state);
        return FW_OK;

    case FW_TASK_CTRL_RESUME:
        if (!controllable) return FW_ENOTSUP;
        if (!user_suspended) return FW_EBUSY;
        if (manager->port->resume(manager->port->ctx, task->os_task) != FW_OK)
            return FW_EIO;
        state = critical_enter(manager);
        task->user_suspended = false;
        task->last_heartbeat_ms = manager_now_ms(manager);
        critical_exit(manager, state);
        return FW_OK;

    case FW_TASK_CTRL_SET_PRIORITY:
        if (!controllable) return FW_ENOTSUP;
        if (value >= manager->priority_max) return FW_EINVAL;
        if (manager->port->set_priority(manager->port->ctx, task->os_task,
                                        value) != FW_OK) return FW_EIO;
        task->priority = value;
        return FW_OK;

    default:
        return FW_EINVAL;
    }
}

void fw_task_cpu_tick(void)
{
    fw_task_manager_t *manager = g_task_manager;
    void *current;
    void *idle;
    size_t i;

    if (manager == NULL || !manager->initialized) return;
    ++manager->cpu_total_ticks;

    current = manager->port->current_thread(manager->port->ctx);
    if (current == NULL) {
        ++manager->cpu_other_ticks;
        return;
    }

    idle = manager->port->idle_thread(manager->port->ctx);
    if (current == idle) {
        ++manager->cpu_idle_ticks;
        return;
    }

    for (i = 0U; i < manager->count; ++i) {
        fw_task_t *task = &manager->tasks[i];
        if (task->started && task->os_task == current) {
            ++task->cpu_ticks;
            return;
        }
    }
    ++manager->cpu_other_ticks;
}
