/*
 * rtthread_clock.c - RT-Thread tick 到 fw_clock_t 的适配。
 *
 * 层级：src/framework/port/rtthread
 * 职责：
 *   1. 提供单调毫秒时间；
 *   2. 不向上层暴露 rtthread.h。
 * 上下文：now_ms 可在任务/ISR 读取。
 * 依赖：fw/rtthread_clock.h、rtthread.h。
 *
 * 实现说明：
 *   rt_tick_get() 返回 rt_tick_t，在 32 位平台上通常是 uint32_t，
 *   会按 2^32 回绕。这里先转成 uint64_t 再乘 1000，
 *   避免 tick 较大时乘法溢出；最后截断回 uint32_t，
 *   与 fw_clock_t 的“按 2^32 回绕”约定一致。
 */
#include "fw/rtthread_clock.h"
#include <rtthread.h>

/*
 * 读取当前毫秒数。
 *
 * 计算：
 *   ms = tick * 1000 / RT_TICK_PER_SECOND
 *
 * 先做 64 位乘法：当 RT_TICK_PER_SECOND=1000 时结果等于 tick；
 * 如果以后改成 100 Hz，仍能正确换算且不会在 tick 较大时溢出。
 * ctx 未使用，适配层没有额外状态。
 */
static uint32_t rt_now_ms(void *ctx)
{
    (void)ctx;
    return (uint32_t)(((uint64_t)rt_tick_get() * 1000U) / RT_TICK_PER_SECOND);
}

/*
 * 构造时钟对象。
 * 返回结构体而不是全局指针，调用者可以按需保存/传递；
 * 函数指针指向静态函数，生命周期覆盖整个程序。
 */
fw_clock_t fw_rtthread_clock(void)
{
    fw_clock_t clock = { rt_now_ms, RT_NULL };
    return clock;
}
