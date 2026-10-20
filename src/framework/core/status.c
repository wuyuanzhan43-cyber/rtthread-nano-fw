/*
 * status.c - 状态码的可读名称。
 *
 * 层级：src/framework/core
 * 职责：为 rt_kprintf、Host 测试输出和调试器提供稳定的状态字符串。
 * 上下文：纯函数，无共享状态，可在任务或中断中调用。
 * 依赖：fw/status.h。
 */
#include "fw/status.h"

/*
 * 状态码到字符串的映射。
 *
 * 这里刻意不使用数组下标查表：
 *   - 状态码是负数，直接做下标需要额外偏移；
 *   - switch 更直观，编译器通常会生成紧凑的跳转表；
 *   - 新增状态码时漏写分支会落到 default，便于发现遗漏。
 */
const char *fw_status_name(fw_status_t status)
{
    switch (status) {
    case FW_OK: return "OK";
    case FW_EINVAL: return "EINVAL";
    case FW_EAGAIN: return "EAGAIN";
    case FW_EIO: return "EIO";
    case FW_ETIMEOUT: return "ETIMEOUT";
    case FW_EOVERFLOW: return "EOVERFLOW";
    case FW_ENOTREADY: return "ENOTREADY";
    case FW_ENOTSUP: return "ENOTSUP";
    case FW_ENODEV: return "ENODEV";
    case FW_EEXIST: return "EEXIST";
    case FW_EBUSY: return "EBUSY";
    default: return "UNKNOWN";
    }
}
