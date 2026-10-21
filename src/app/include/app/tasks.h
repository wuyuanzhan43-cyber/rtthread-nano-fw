/*
 * app/tasks.h - 应用任务装配接口。
 *
 * 层级：src/app/task（公共头位于 src/app/include/app）
 * 职责：
 *   1. 初始化任务管理器并 adopt main；
 *   2. 创建/启动 app_rpc 静态任务；
 *   3. 向 main 暴露 manager 与 app_rpc 实例。
 *
 * 上下文：app_tasks_init/start 在 main 线程、调度器启动后调用。
 * 依赖：fw/task.h、app/rpc.h。
 */
#ifndef APP_TASKS_H
#define APP_TASKS_H

#include "fw/task.h"
#include "app/rpc.h"

/*
 * 初始化 manager、adopt main、创建 RPC 任务（不启动）。
 * 返回 FW_OK 或端口/内存错误。
 */
fw_status_t app_tasks_init(void);

/* 启动 app_rpc 任务。 */
fw_status_t app_tasks_start(void);

/* 任务管理器；app_tasks_init 之前返回的对象尚未初始化。 */
fw_task_manager_t *app_tasks_manager(void);

/* 静态 app_rpc 实例；交给 app_rpc_init 使用。 */
app_rpc_t *app_tasks_rpc(void);

#endif
