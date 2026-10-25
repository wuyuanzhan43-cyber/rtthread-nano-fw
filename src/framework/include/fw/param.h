/*
 * fw/param.h - 双槽 A/B 参数存储。
 *
 * 层级：src/framework/core
 * 职责：
 *   1. 在任意可擦写后端上维护两个参数槽；
 *   2. 用槽头 + CRC32 + 递增序号选择最新有效数据；
 *   3. 单槽写入失败/掉电时自动回退到另一个槽。
 *
 * 上下文：任务上下文；save 会触发擦除/编程，可能阻塞。
 * 依赖：fw/status.h。
 *
 * 后端约束：
 *   - 两个槽必须能独立擦除（通常各占一个 Flash 扇区）；
 *   - slot_size >= FW_PARAM_HEADER_SIZE + data_max，且 4 字节对齐；
 *   - program 的 offset/len 都是 4 字节对齐，len 可以跨任意多个字。
 *
 * 只有一个参数实例；状态保存在静态变量中，不分配堆。
 */
#ifndef FW_PARAM_H
#define FW_PARAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "fw/status.h"

/* 由 Kconfig 生成；独立编译时给安全默认值。 */
#ifndef FW_PARAM_DATA_MAX
#define FW_PARAM_DATA_MAX 1024
#endif

#define FW_PARAM_MAGIC 0x46575052U   /* "FWPR" */
#define FW_PARAM_VERSION 1U
#define FW_PARAM_HEADER_SIZE 20U     /* magic/version/seq/size/crc，各 4 字节 */

/*
 * 存储后端。
 *
 * read    - 从分区偏移 offset 读 len 字节；
 * program - 从分区偏移 offset 编程 len 字节，offset/len 4 字节对齐；
 * erase   - 擦除 [offset, offset+len)；
 * ctx     - 后端私有数据（平台 NVM 后端可以为 NULL）。
 */
typedef struct {
    int (*read)(void *ctx, uint32_t offset, void *buf, size_t len);
    int (*program)(void *ctx, uint32_t offset, const void *buf, size_t len);
    int (*erase)(void *ctx, uint32_t offset, size_t len);
    size_t slot_size;
    void *ctx;
} fw_param_backend_t;

/*
 * 初始化参数存储。
 *
 * 扫描两个槽，选择序号最高且 CRC 有效的一份；没有有效槽也算初始化成功，
 * 后续 load 返回 FW_ENODATA。data_max 不能超过槽容量。
 */
fw_status_t fw_param_init(const fw_param_backend_t *backend, size_t data_max);

/*
 * 读取最新参数。
 *
 * size >= 存储长度时复制数据并返回 FW_OK；
 * size 不足时返回 FW_EOVERFLOW，并通过 out_size 告诉调用者所需长度。
 * 没有有效槽返回 FW_ENODATA。
 */
fw_status_t fw_param_load(void *data, size_t size, size_t *out_size);

/*
 * 保存参数。
 *
 * 擦除非活动槽 -> 写槽头 -> 写数据 -> 切换活动槽。
 * 掉电发生在任何一步，另一个槽仍然有效；下一次 init 会回退。
 */
fw_status_t fw_param_save(const void *data, size_t size);

/* 擦除两个槽并清空内存状态。 */
fw_status_t fw_param_clear(void);

/* 当前 data_max；未初始化返回 0。 */
size_t fw_param_capacity(void);

/* 是否有一份有效参数。 */
bool fw_param_valid(void);

#endif
