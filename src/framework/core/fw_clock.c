/*
 * fw_clock.c - 时间差计算与可注入延时。
 *
 * 层级：src/framework/core
 * 职责：
 *   1. 提供无符号回绕安全的毫秒差值；
 *   2. 保存并调用平台延时钩子。
 * 上下文：delay 钩子只在任务上下文调用；本文件无共享缓冲。
 * 依赖：fw/clock.h、stddef.h。
 *
 * 说明：
 *   delay_fn 是唯一的全局状态，在 board 初始化阶段写入一次，
 *   运行期只读。当前项目没有并发安装延时的需求，因此不加锁。
 */
#include "fw/clock.h"
#include <stddef.h>

/* 平台延时实现；NULL 表示未安装，fw_delay_ms 直接返回。 */
static fw_delay_fn_t delay_fn;

/*
 * 计算经过的毫秒数。
 *
 * 直接相减即可：uint32_t 的减法在 2^32 回绕时仍然得到正确的
 * 小差值（模运算）。前提是真实间隔小于 2^31 ms。
 */
uint32_t fw_elapsed_ms(uint32_t now, uint32_t then)
{
    return now - then;
}

/* 安全读取当前毫秒；未提供时钟时返回 0，避免调用者判空。 */
uint32_t fw_clock_now_ms(const fw_clock_t *clock)
{
    if (clock == NULL || clock->now_ms == NULL) return 0U;
    return clock->now_ms(clock->ctx);
}

/* 安装延时钩子。board 初始化时调用；可以重复安装，后装覆盖先装。 */
void fw_clock_set_delay_fn(fw_delay_fn_t fn)
{
    delay_fn = fn;
}

/*
 * 毫秒延时。
 *
 * ms==0 时不调用钩子，避免某些平台的 tmdelay(0) 产生一次调度。
 * 未安装钩子时静默返回：Host 测试不关心真实时间，固件启动早期
 * 也不会在安装前调用需要延时的面板初始化。
 */
void fw_delay_ms(uint32_t ms)
{
    if (delay_fn != NULL && ms != 0U) delay_fn(ms);
}
