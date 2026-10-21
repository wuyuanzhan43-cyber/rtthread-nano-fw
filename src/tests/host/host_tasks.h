/*
 * host_tasks.h - Host 测试用的 fake 任务端口。
 *
 * 层级：src/tests/host
 * 职责：
 *   1. 实现 fw_task_port_ops_t，不依赖 RTOS；
 *   2. 让测试控制任务状态、栈使用、优先级和当前线程；
 *   3. 复用 host 假时钟，模拟 SysTick 的 fw_task_cpu_tick。
 *
 * 上下文：单线程测试。
 * 依赖：fw/task.h、fw/clock.h。
 */
#ifndef HOST_TASKS_H
#define HOST_TASKS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "fw/task.h"
#include "fw/clock.h"

typedef struct host_task {
    void *handle;
    uint8_t state;
    uint8_t priority;
    uint32_t stack_size;
    uint32_t stack_used;
    bool closed;
} host_task_t;

typedef struct host_tasks {
    host_task_t tasks[FW_TASK_MAX];
    size_t count;

    void *current;
    void *idle;
    uint8_t idle_marker;

    fw_task_port_ops_t ops;
    fw_task_manager_t manager;
} host_tasks_t;

/* 初始化 fake 端口和 manager；refresh_ms 为健康刷新周期。 */
fw_status_t host_tasks_init(host_tasks_t *host, fw_clock_t clock,
                            uint32_t refresh_ms);

/* 清理 manager 的全局指针；不会释放任何内存。 */
void host_tasks_deinit(host_tasks_t *host);

/* 设置第 index 个任务的栈高水位/总大小。 */
void host_task_set_stack(host_tasks_t *host, size_t index, uint32_t used,
                         uint32_t size);

/* 设置第 index 个任务的线程状态。 */
void host_task_set_state(host_tasks_t *host, size_t index, uint8_t state);

/* 把第 index 个任务设为当前线程。 */
void host_task_set_current(host_tasks_t *host, size_t index);

/* 把当前线程设为 idle。 */
void host_task_set_current_idle(host_tasks_t *host);

/* 把当前线程设为未托管线程。 */
void host_task_set_current_none(host_tasks_t *host);

/* 模拟一次 SysTick 采样。 */
void host_task_cpu_tick(host_tasks_t *host);

#endif
