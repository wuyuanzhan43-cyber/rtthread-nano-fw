/*
 * fw/log.h - 最小分级日志。
 *
 * 层级：src/framework/core
 * 职责：
 *   1. 提供与 OS 无关的日志级别和输出 sink；
 *   2. 通过 FW_LOG_LEVEL 做编译期裁剪；
 *   3. 运行期可降低级别，但不能超过编译期上限。
 *
 * 上下文：任务上下文；内部使用静态行缓冲，不可重入/不可在 ISR 使用。
 * 依赖：fw_config.h、stdarg.h。
 *
 * sink 由 port 绑定，例如 RT-Thread 端口绑定到 rt_hw_console_output。
 */
#ifndef FW_LOG_H
#define FW_LOG_H

#include <stdarg.h>
#include "fw_config.h"

/* FW_LOG_LEVEL 来自 Kconfig；独立编译时默认关闭。 */
#ifndef FW_LOG_LEVEL
#define FW_LOG_LEVEL 0
#endif

typedef enum {
    FW_LOG_OFF = 0,
    FW_LOG_ERROR = 1,
    FW_LOG_WARN = 2,
    FW_LOG_INFO = 3,
    FW_LOG_DEBUG = 4
} fw_log_level_t;

typedef void (*fw_log_sink_t)(const char *line);

/* 绑定输出；NULL 表示丢弃。 */
void fw_log_set_sink(fw_log_sink_t sink);

/* 设置运行期级别；超过 FW_LOG_LEVEL 的值会被钳位。 */
void fw_log_set_level(fw_log_level_t level);

fw_log_level_t fw_log_get_level(void);

/*
 * 格式化并输出一行。
 *
 * 输出格式：[E] tag: message\n
 * 行缓冲固定 128 字节，超长截断；sink 为空或级别不足时直接返回。
 */
void fw_log_printf(fw_log_level_t level, const char *tag,
                   const char *fmt, ...);

#if FW_LOG_LEVEL >= 1
#define FW_LOGE(tag, ...) fw_log_printf(FW_LOG_ERROR, tag, __VA_ARGS__)
#else
#define FW_LOGE(tag, ...) ((void)0)
#endif

#if FW_LOG_LEVEL >= 2
#define FW_LOGW(tag, ...) fw_log_printf(FW_LOG_WARN, tag, __VA_ARGS__)
#else
#define FW_LOGW(tag, ...) ((void)0)
#endif

#if FW_LOG_LEVEL >= 3
#define FW_LOGI(tag, ...) fw_log_printf(FW_LOG_INFO, tag, __VA_ARGS__)
#else
#define FW_LOGI(tag, ...) ((void)0)
#endif

#if FW_LOG_LEVEL >= 4
#define FW_LOGD(tag, ...) fw_log_printf(FW_LOG_DEBUG, tag, __VA_ARGS__)
#else
#define FW_LOGD(tag, ...) ((void)0)
#endif

#endif
