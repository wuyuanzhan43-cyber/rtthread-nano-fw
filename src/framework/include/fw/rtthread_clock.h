/*
 * fw/rtthread_clock.h - RT-Thread 毫秒时钟适配。
 *
 * 层级：src/framework/port/rtthread（公共头位于 src/framework/include/fw）
 * 职责：把 rt_tick_get() 包装成 fw_clock_t，供业务层使用。
 * 上下文：返回的 now_ms 可在任务或 ISR 中读取。
 * 依赖：fw/clock.h。
 *
 * 设计说明：
 *   这是 framework 中少数允许包含 RT-Thread 头文件的目录之一。
 *   core/components 只看到 fw_clock_t 函数指针，看不到 rtthread.h；
 *   Host 测试可以用自己的假时钟替换它，因此业务逻辑不需要 RTOS。
 */
#ifndef FW_RTTHREAD_CLOCK_H
#define FW_RTTHREAD_CLOCK_H

#include "fw/clock.h"

/*
 * 返回一个由 RT-Thread tick 驱动的毫秒时钟。
 *
 * 返回结构体按值传递，内部 ctx 为 RT_NULL；
 * 调用者可以把它保存到自己的配置结构体中长期使用。
 *
 * 精度：受 RT_TICK_PER_SECOND 限制，当前配置为 1000 Hz，
 * 因此 1 tick = 1 ms，转换后没有额外截断误差。
 */
fw_clock_t fw_rtthread_clock(void);

#endif
