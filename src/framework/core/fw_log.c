/*
 * fw_log.c - 最小分级日志实现。
 *
 * 层级：src/framework/core
 * 职责：
 *   1. 维护运行期级别和 sink；
 *   2. 用固定行缓冲格式化，不动态分配；
 *   3. 通过 FW_LOG_LEVEL 限制运行期级别上限。
 *
 * 上下文：任务上下文；静态缓冲不可重入。
 * 依赖：fw/log.h、stdio.h、string.h。
 */
#include "fw/log.h"
#include <stdio.h>
#include <string.h>

static fw_log_sink_t s_sink;
static fw_log_level_t s_level = (fw_log_level_t)FW_LOG_LEVEL;

static char level_char(fw_log_level_t level)
{
    switch (level) {
    case FW_LOG_ERROR: return 'E';
    case FW_LOG_WARN: return 'W';
    case FW_LOG_INFO: return 'I';
    case FW_LOG_DEBUG: return 'D';
    default: return '?';
    }
}

void fw_log_set_sink(fw_log_sink_t sink)
{
    s_sink = sink;
}

void fw_log_set_level(fw_log_level_t level)
{
    if ((int)level > FW_LOG_LEVEL) level = (fw_log_level_t)FW_LOG_LEVEL;
    s_level = level;
}

fw_log_level_t fw_log_get_level(void)
{
    return s_level;
}

void fw_log_printf(fw_log_level_t level, const char *tag,
                   const char *fmt, ...)
{
    char line[128];
    va_list args;
    int prefix;
    int length;

    if (s_sink == NULL || fmt == NULL) return;
    if ((int)level <= (int)FW_LOG_OFF) return;
    if ((int)level > (int)s_level) return;

    prefix = snprintf(line, sizeof(line), "[%c] %s: ", level_char(level),
                      tag != NULL ? tag : "-");
    if (prefix < 0) return;
    if ((size_t)prefix >= sizeof(line)) prefix = (int)sizeof(line) - 1;

    va_start(args, fmt);
    (void)vsnprintf(&line[prefix], sizeof(line) - (size_t)prefix, fmt, args);
    va_end(args);

    length = (int)strlen(line);
    if (length > 0 && line[length - 1] == '\n') {
        /* 调用者已经带换行。 */
    } else if ((size_t)length < sizeof(line) - 1U) {
        line[length++] = '\n';
        line[length] = '\0';
    }

    s_sink(line);
}
