/*
 * fw/status.h - 框架统一状态码。
 *
 * 层级：src/framework/core
 * 职责：为 core、components、drivers 和 tests 提供同一套错误语义，
 *       避免每个模块自定义一套正负返回值。
 * 上下文：纯类型和常量定义，不涉及共享状态。
 * 依赖：stdint.h。
 *
 * 使用约定：
 *   - 成功返回 0 或非负的字节数/尺寸；
 *   - 失败返回负的 fw_status_t；
 *   - 调用者用 result < 0 判断失败，不要用 result != FW_OK，
 *     因为 read/write 成功时返回的是“实际传输字节数”。
 *
 * 为什么使用负值枚举：
 *   - 与 POSIX/Linux 的错误码习惯一致；
 *   - 可以直接与“返回字节数”的函数共用 int 返回类型；
 *   - 枚举显式写成负数，避免出现 -FW_EINVAL 反而变成正数的问题。
 */
#ifndef FW_STATUS_H
#define FW_STATUS_H

#include <stdint.h>

/* 等待超时约定：UINT32_MAX 表示永久等待，0 表示完全不等待。 */
#define FW_WAIT_FOREVER UINT32_MAX
#define FW_NO_WAIT 0U

/*
 * 状态码。
 *
 * 这些值在日志、Host 测试断言和 HIL 结果中都会出现；
 * 新增错误码时只能追加，不能修改已有值的数值。
 */
typedef enum {
    FW_OK = 0,          /* 操作按要求完成 */
    FW_EINVAL = -1,     /* 参数、配置或调用顺序非法 */
    FW_EAGAIN = -2,     /* 暂时性背压；稍后重试同一工作 */
    FW_EIO = -3,        /* 设备/链路错误；连续性可能已丢失 */
    FW_ETIMEOUT = -4,   /* 操作未在期限内完成 */
    FW_EOVERFLOW = -5,  /* 有界缓冲/流溢出；已有数据被丢弃 */
    FW_ENOTREADY = -6,  /* 设备存在但尚未初始化/启动 */
    FW_ENOTSUP = -7,    /* 当前实现不支持该操作 */
    FW_ENODEV = -8,     /* 注册表中不存在该设备 */
    FW_EEXIST = -9,     /* 设备名重复注册 */
    FW_EBUSY = -10,     /* 设备忙或注册表已封存 */
    FW_ENODATA = -11    /* 参数/持久化存储中还没有有效数据 */
} fw_status_t;

/*
 * 把状态码转成稳定的可读字符串。
 * 未知值返回 "UNKNOWN"，永不返回 NULL，因此可以直接用于 printf。
 * 返回的字符串位于只读数据段，调用者不要释放或修改。
 */
const char *fw_status_name(fw_status_t status);

#endif
