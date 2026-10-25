/*
 * fw/rtthread_log.h - 把 fw_log sink 绑定到 RT-Thread 控制台。
 *
 * 层级：src/framework/port/rtthread
 * 职责：提供 OS 适配入口；framework/core 不直接依赖 RT-Thread。
 */
#ifndef FW_RTTHREAD_LOG_H
#define FW_RTTHREAD_LOG_H

/* 将 fw_log 输出接到 rt_hw_console_output；main 启动时调用一次。 */
void fw_rtthread_log_bind(void);

#endif
