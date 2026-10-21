/*
 * fw/rtthread_task.h - RT-Thread 任务端口。
 *
 * 层级：src/framework/include/fw（实现位于 src/framework/port/rtthread）
 * 职责：把 RT-Thread 的静态线程 API 适配成 fw_task_port_ops_t。
 * 上下文：port 的 create/adopt/start/control 在任务上下文调用；
 *         current_thread/idle_thread 在 SysTick 中断中调用。
 * 依赖：fw/task.h。
 */
#ifndef FW_RTTHREAD_TASK_H
#define FW_RTTHREAD_TASK_H

#include "fw/task.h"

/*
 * 返回 RT-Thread 任务端口。
 *
 * 端口内部维护 FW_TASK_MAX 个静态 struct rt_thread 槽位；
 * 只支持单实例，且必须在调度器启动后、任务上下文初始化 manager。
 */
const fw_task_port_ops_t *fw_rtthread_task_port(void);

#endif
