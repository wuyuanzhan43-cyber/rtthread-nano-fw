/*
 * rtthread_task.c - RT-Thread 静态线程到 fw_task_port_ops_t 的适配。
 *
 * 层级：src/framework/port/rtthread
 * 职责：
 *   1. 静态 rt_thread 槽位与 create/adopt/start/control；
 *   2. 状态、优先级、栈高水位快照；
 *   3. 当前/idle 线程查询与短临界区；
 *   4. 为框架提供 RT_THREAD_PRIORITY_MAX。
 *
 * 上下文：
 *   - create/adopt/start/suspend/resume/set_priority/snapshot：任务上下文；
 *   - current_thread/idle_thread：SysTick 中断中调用，必须极短；
 *   - critical_enter/exit：rt_hw_interrupt_disable/enable。
 *
 * 依赖：fw/rtthread_task.h、rtthread.h、rthw.h、string.h。
 *
 * 栈高水位：
 *   RT-Thread 的 _rt_thread_init 会把整个用户栈填成 '#'（0x23）。
 *   从栈底向上按 word 扫描连续的 0x23232323，再逐字节处理尾部；
 *   用 sp 计算一个下界并取较大值，缓解已用数据中恰好出现 '#' 的少报。
 */
#include "fw/rtthread_task.h"
#include <rtthread.h>
#include <rthw.h>
#include <string.h>

/* 静态线程对象槽位；RT_USING_HEAP=n 时不能使用 rt_thread_create。 */
static struct rt_thread s_threads[FW_TASK_MAX];
static bool s_used[FW_TASK_MAX];

/* 根据已复制的 stat 和当前线程映射状态；Nano 不会设置 RUNNING。 */
static uint8_t map_state(rt_thread_t thread, rt_uint8_t stat)
{
    if (thread == rt_thread_self() && stat == RT_THREAD_READY)
        return FW_TASK_STATE_RUNNING;

    switch (stat) {
    case RT_THREAD_INIT:    return FW_TASK_STATE_INIT;
    case RT_THREAD_READY:   return FW_TASK_STATE_READY;
    case RT_THREAD_SUSPEND: return FW_TASK_STATE_SUSPENDED;
    case RT_THREAD_CLOSE:   return FW_TASK_STATE_CLOSED;
    default:                return FW_TASK_STATE_UNKNOWN;
    }
}

/* 扫描内核预填的 '#'，返回近似历史最大栈使用量。 */
static uint32_t stack_high_water(const void *stack_addr, uint32_t stack_size,
                                 const void *sp)
{
    const uint8_t *base = (const uint8_t *)stack_addr;
    uint32_t untouched = 0U;
    uint32_t used;
    uint32_t sp_used = 0U;
    const uint8_t *high;
    const uint8_t *stack_pointer = (const uint8_t *)sp;

    if (base == NULL || stack_size == 0U) return 0U;
    high = base + stack_size;

    while (untouched + 4U <= stack_size) {
        uint32_t word;
        memcpy(&word, base + untouched, sizeof(word));
        if (word != 0x23232323U) break;
        untouched += 4U;
    }
    while (untouched < stack_size && base[untouched] == '#') ++untouched;

    used = stack_size - untouched;
    if (stack_pointer != NULL && stack_pointer >= base && stack_pointer <= high)
        sp_used = (uint32_t)(high - stack_pointer);
    if (sp_used > used) used = sp_used;
    if (used > stack_size) used = stack_size;
    return used;
}

static fw_status_t port_create(void *ctx, const fw_task_create_config_t *config,
                               void **os_task)
{
    size_t slot;

    (void)ctx;
    if (config == NULL || os_task == NULL || config->name == NULL ||
        config->stack == NULL || config->stack_size < 256U ||
        ((uintptr_t)config->stack % RT_ALIGN_SIZE) != 0U ||
        config->priority >= RT_THREAD_PRIORITY_MAX ||
        config->entry == NULL) return FW_EINVAL;

    for (slot = 0U; slot < FW_TASK_MAX; ++slot) {
        if (!s_used[slot]) break;
    }
    if (slot >= FW_TASK_MAX) return FW_EOVERFLOW;

    if (rt_thread_init(&s_threads[slot], config->name, config->entry,
                       config->arg, config->stack,
                       (rt_uint32_t)config->stack_size, config->priority,
                       config->time_slice) != RT_EOK) return FW_EIO;

    s_used[slot] = true;
    *os_task = &s_threads[slot];
    return FW_OK;
}

static fw_status_t port_adopt(void *ctx, const fw_task_adopt_config_t *config,
                              void **os_task)
{
    rt_thread_t thread;

    (void)ctx;
    if (config == NULL || config->name == NULL || os_task == NULL)
        return FW_EINVAL;

    thread = rt_thread_find((char *)config->name);
    if (thread == RT_NULL && rt_thread_self() != RT_NULL &&
        strcmp(rt_thread_self()->name, config->name) == 0) {
        thread = rt_thread_self();
    }
    if (thread == RT_NULL) return FW_ENODEV;

    *os_task = thread;
    return FW_OK;
}

static fw_status_t port_start(void *ctx, void *os_task)
{
    (void)ctx;
    if (os_task == NULL) return FW_EINVAL;
    return rt_thread_startup((rt_thread_t)os_task) == RT_EOK ? FW_OK : FW_EIO;
}

static fw_status_t port_suspend(void *ctx, void *os_task)
{
    rt_thread_t thread = (rt_thread_t)os_task;

    (void)ctx;
    if (thread == NULL) return FW_EINVAL;
    /* 协作挂起只由目标任务自己调用。 */
    if (thread != rt_thread_self()) return FW_EBUSY;
    if (rt_thread_suspend(thread) != RT_EOK) return FW_EBUSY;
    rt_schedule();
    return FW_OK;
}

static fw_status_t port_resume(void *ctx, void *os_task)
{
    (void)ctx;
    if (os_task == NULL) return FW_EINVAL;
    if (rt_thread_resume((rt_thread_t)os_task) != RT_EOK) return FW_EIO;
    rt_schedule();
    return FW_OK;
}

static fw_status_t port_set_priority(void *ctx, void *os_task, uint8_t priority)
{
    uint8_t value = priority;

    (void)ctx;
    if (os_task == NULL || priority >= RT_THREAD_PRIORITY_MAX)
        return FW_EINVAL;
    if (rt_thread_control((rt_thread_t)os_task,
                          RT_THREAD_CTRL_CHANGE_PRIORITY,
                          &value) != RT_EOK) return FW_EIO;
    return FW_OK;
}

static fw_status_t port_snapshot(void *ctx, void *os_task,
                                 fw_task_os_info_t *info)
{
    rt_thread_t thread = (rt_thread_t)os_task;
    rt_base_t level;
    rt_uint8_t stat;
    rt_uint8_t priority;
    void *stack_addr;
    rt_uint32_t stack_size;
    void *sp;
    uint32_t used;

    (void)ctx;
    if (thread == NULL || info == NULL) return FW_EINVAL;

    level = rt_hw_interrupt_disable();
    stat = (rt_uint8_t)(thread->stat & RT_THREAD_STAT_MASK);
    priority = thread->current_priority;
    stack_addr = thread->stack_addr;
    stack_size = thread->stack_size;
    sp = thread->sp;
    rt_hw_interrupt_enable(level);

    used = stack_high_water(stack_addr, stack_size, sp);
    info->state = map_state(thread, stat);
    info->priority = priority;
    info->stack_size = stack_size;
    info->stack_used = used;
    info->stack_free = stack_size >= used ? stack_size - used : 0U;
    info->closed = stat == RT_THREAD_CLOSE;
    return FW_OK;
}

static void *port_current_thread(void *ctx)
{
    (void)ctx;
    return rt_thread_self();
}

static void *port_idle_thread(void *ctx)
{
    (void)ctx;
    return rt_thread_idle_gethandler();
}

static uint16_t port_max_priority(void *ctx)
{
    (void)ctx;
    return RT_THREAD_PRIORITY_MAX;
}

static uint32_t port_critical_enter(void *ctx)
{
    (void)ctx;
    return (uint32_t)rt_hw_interrupt_disable();
}

static void port_critical_exit(void *ctx, uint32_t state)
{
    (void)ctx;
    rt_hw_interrupt_enable((rt_base_t)state);
}

static const fw_task_port_ops_t rtt_task_ops = {
    port_create,
    port_adopt,
    port_start,
    port_suspend,
    port_resume,
    port_set_priority,
    port_snapshot,
    port_current_thread,
    port_idle_thread,
    port_max_priority,
    port_critical_enter,
    port_critical_exit,
    NULL
};

const fw_task_port_ops_t *fw_rtthread_task_port(void)
{
    return &rtt_task_ops;
}
