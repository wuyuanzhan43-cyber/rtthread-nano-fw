/*
 * fw/clock.h - 单调毫秒时钟与平台延时。
 *
 * 层级：src/framework/core
 * 职责：
 *   1. 让业务层只依赖“毫秒时间”接口，不直接包含 RT-Thread 头文件；
 *   2. 提供按 2^32 回绕的时间差计算；
 *   3. 提供可注入的延时钩子，Host 测试可以替换为计数实现。
 *
 * 上下文：
 *   - now_ms 可在任务或 ISR 中读取（具体实现必须是无锁的）；
 *   - fw_delay_ms 只在任务上下文调用，不能在 ISR 中阻塞。
 *
 * 依赖：stdint.h。
 *
 * 为什么需要这层适配：
 *   demo_app 需要周期性上报和半帧超时判断，但它不应该知道
 *   rt_tick_get() 或 RT_TICK_PER_SECOND。通过 fw_clock_t 注入，
 *   Host 测试可以传入假时钟，精确控制时间推进，不依赖真实延时。
 */
#ifndef FW_CLOCK_H
#define FW_CLOCK_H

#include <stdint.h>

/*
 * 时钟接口。
 *
 * now_ms - 返回单调递增的毫秒计数，允许在 2^32 处回绕；
 * ctx    - 传给 now_ms 的上下文，RT-Thread 端口传 RT_NULL。
 */
typedef struct {
    uint32_t (*now_ms)(void *ctx);
    void *ctx;
} fw_clock_t;

/*
 * 计算经过的毫秒数。
 *
 * 依赖无符号整数回绕：now - then 在 2^32 回绕时仍然得到正确的小差值。
 * 前提：两次采样的间隔小于 2^31 ms（约 24.8 天），
 * 对本项目的上报周期和超时判断来说远远足够。
 */
uint32_t fw_elapsed_ms(uint32_t now, uint32_t then);

/* 读取时钟；clock 或 now_ms 为空时返回 0，便于测试降级。 */
uint32_t fw_clock_now_ms(const fw_clock_t *clock);

/*
 * 延时钩子。
 *
 * 平台初始化时安装；未安装时 fw_delay_ms 为空操作，
 * 这样纯逻辑测试不会真的等待，也不会因为缺少 RTOS 而链接失败。
 */
typedef void (*fw_delay_fn_t)(uint32_t ms);
void fw_clock_set_delay_fn(fw_delay_fn_t fn);
void fw_delay_ms(uint32_t ms);

#endif
