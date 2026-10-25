/*
 * rtthread_log.c - fw_log 的 RT-Thread 输出端口。
 *
 * 层级：src/framework/port/rtthread
 * 职责：把格式化后的整行交给 rt_hw_console_output。
 * 上下文：任务上下文；控制台实现负责 ring/轮询细节。
 */
#include "fw/log.h"
#include "fw/rtthread_log.h"
#include <rthw.h>

static void rt_log_sink(const char *line)
{
    rt_hw_console_output(line);
}

void fw_rtthread_log_bind(void)
{
    fw_log_set_sink(rt_log_sink);
}
