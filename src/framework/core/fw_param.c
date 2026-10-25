/*
 * fw_param.c - 双槽 A/B 参数存储实现。
 *
 * 层级：src/framework/core
 * 职责：
 *   1. 扫描两个槽的槽头，选择序号最高且 CRC 有效的一份；
 *   2. save 时擦除非活动槽、写槽头和数据，然后切换活动槽；
 *   3. load 时重新校验 CRC，防止运行期数据被破坏。
 *
 * 上下文：任务上下文；擦除/编程期间可能阻塞。
 * 依赖：fw/param.h、string.h。
 *
 * 掉电语义：
 *   - 擦除/编程发生在非活动槽，活动槽始终保持可读；
 *   - 新槽只有在槽头和数据 CRC 都通过后才成为活动槽；
 *   - 若新槽写坏，下一次 init 会回退到旧槽。
 */
#include "fw/param.h"
#include <string.h>

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t sequence;
    uint32_t size;
    uint32_t crc;
} param_header_t;

/* 槽头 CRC 计算时的临时副本；保证 CRC 字段本身按 0 参与。 */
typedef char param_header_size_check[(sizeof(param_header_t) ==
                                      FW_PARAM_HEADER_SIZE) ? 1 : -1];

static const fw_param_backend_t *s_backend;
static size_t s_data_max;
static int s_active_slot = -1;
static bool s_valid;
static uint32_t s_next_sequence = 1U;

/* CRC-32 (IEEE 802.3, 反射多项式 0xEDB88320)。 */
static uint32_t crc32_begin(void)
{
    return 0xFFFFFFFFU;
}

static uint32_t crc32_add(uint32_t crc, const void *data, size_t length)
{
    const uint8_t *bytes = (const uint8_t *)data;
    size_t i;
    int bit;

    for (i = 0U; i < length; ++i) {
        crc ^= bytes[i];
        for (bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
        }
    }
    return crc;
}

static uint32_t crc32_value(const void *header, const void *data,
                            size_t length)
{
    uint32_t crc = crc32_begin();
    crc = crc32_add(crc, header, FW_PARAM_HEADER_SIZE);
    if (length != 0U) crc = crc32_add(crc, data, length);
    return ~crc;
}

static uint32_t slot_offset(int slot)
{
    return (uint32_t)((size_t)slot * s_backend->slot_size);
}

/* 读取槽头并做基本字段检查；不校验数据 CRC。 */
static fw_status_t read_header(int slot, param_header_t *header)
{
    uint32_t offset = slot_offset(slot);
    int result = s_backend->read(s_backend->ctx, offset, header,
                                 sizeof(*header));
    if (result != FW_OK) return (fw_status_t)result;
    if (header->magic != FW_PARAM_MAGIC) return FW_ENODATA;
    if (header->version != FW_PARAM_VERSION) return FW_ENODATA;
    if (header->size > s_data_max) return FW_ENODATA;
    return FW_OK;
}

/*
 * 流式校验槽数据 CRC，避免为 data_max 分配一整块静态缓冲。
 * 读失败或长度非法都视为无效。
 */
static bool slot_crc_valid(int slot, const param_header_t *header)
{
    param_header_t copy = *header;
    uint8_t chunk[64];
    uint32_t offset;
    uint32_t remaining = header->size;
    uint32_t crc;

    copy.crc = 0U;
    crc = crc32_begin();
    crc = crc32_add(crc, &copy, sizeof(copy));
    offset = slot_offset(slot) + FW_PARAM_HEADER_SIZE;

    while (remaining != 0U) {
        size_t take = remaining < sizeof(chunk) ? remaining : sizeof(chunk);
        int result = s_backend->read(s_backend->ctx, offset, chunk, take);
        if (result != FW_OK) return false;
        crc = crc32_add(crc, chunk, take);
        offset += (uint32_t)take;
        remaining -= (uint32_t)take;
    }
    return (~crc) == header->crc;
}

fw_status_t fw_param_init(const fw_param_backend_t *backend, size_t data_max)
{
    param_header_t header;
    int slot;
    bool found = false;
    uint32_t best_sequence = 0U;
    int best_slot = -1;

    if (backend == NULL || backend->read == NULL ||
        backend->program == NULL || backend->erase == NULL)
        return FW_EINVAL;
    if (backend->slot_size < FW_PARAM_HEADER_SIZE ||
        backend->slot_size % 4U != 0U || data_max == 0U ||
        data_max > backend->slot_size - FW_PARAM_HEADER_SIZE)
        return FW_EINVAL;

    s_backend = backend;
    s_data_max = data_max;
    s_active_slot = -1;
    s_valid = false;
    s_next_sequence = 1U;

    for (slot = 0; slot < 2; ++slot) {
        if (read_header(slot, &header) != FW_OK) continue;
        if (!slot_crc_valid(slot, &header)) continue;
        if (!found || header.sequence > best_sequence) {
            found = true;
            best_sequence = header.sequence;
            best_slot = slot;
        }
    }

    if (found) {
        s_valid = true;
        s_active_slot = best_slot;
        s_next_sequence = best_sequence + 1U;
        if (s_next_sequence == 0U) s_next_sequence = 1U;
    }
    return FW_OK;
}

fw_status_t fw_param_load(void *data, size_t size, size_t *out_size)
{
    param_header_t header;
    fw_status_t status;
    uint32_t offset;

    if (!s_valid || s_active_slot < 0) return FW_ENODATA;
    if (data == NULL) return FW_EINVAL;

    status = read_header(s_active_slot, &header);
    if (status != FW_OK) {
        s_valid = false;
        return FW_ENODATA;
    }
    if (out_size != NULL) *out_size = header.size;
    if (size < header.size) return FW_EOVERFLOW;

    offset = slot_offset(s_active_slot) + FW_PARAM_HEADER_SIZE;
    if (s_backend->read(s_backend->ctx, offset, data, header.size) != FW_OK) {
        s_valid = false;
        return FW_EIO;
    }

    {
        param_header_t copy = header;
        copy.crc = 0U;
        if (crc32_value(&copy, data, header.size) != header.crc) {
            s_valid = false;
            return FW_EIO;
        }
    }
    return FW_OK;
}

/* 编程数据；尾部不足 4 字节时用 0xFF 补齐。 */
static fw_status_t program_data(uint32_t offset, const uint8_t *data,
                                size_t size)
{
    size_t aligned = size & ~(size_t)3U;
    int result;

    if (aligned != 0U) {
        result = s_backend->program(s_backend->ctx, offset, data, aligned);
        if (result != FW_OK) return (fw_status_t)result;
    }
    if (aligned != size) {
        uint8_t tail[4] = {0xFFU, 0xFFU, 0xFFU, 0xFFU};
        memcpy(tail, data + aligned, size - aligned);
        result = s_backend->program(s_backend->ctx, offset + (uint32_t)aligned,
                                    tail, sizeof(tail));
        if (result != FW_OK) return (fw_status_t)result;
    }
    return FW_OK;
}

fw_status_t fw_param_save(const void *data, size_t size)
{
    param_header_t header;
    param_header_t readback;
    uint32_t offset;
    int target;
    int result;

    if (s_backend == NULL || s_data_max == 0U) return FW_ENOTREADY;
    if (data == NULL || size > s_data_max) return FW_EINVAL;

    target = s_valid ? 1 - s_active_slot : 0;
    offset = slot_offset(target);

    result = s_backend->erase(s_backend->ctx, offset, s_backend->slot_size);
    if (result != FW_OK) return (fw_status_t)result;

    header.magic = FW_PARAM_MAGIC;
    header.version = FW_PARAM_VERSION;
    header.sequence = s_next_sequence;
    header.size = (uint32_t)size;
    header.crc = 0U;
    header.crc = crc32_value(&header, data, size);

    result = s_backend->program(s_backend->ctx, offset, &header,
                                sizeof(header));
    if (result != FW_OK) {
        (void)s_backend->erase(s_backend->ctx, offset, s_backend->slot_size);
        return (fw_status_t)result;
    }
    if (program_data(offset + FW_PARAM_HEADER_SIZE, (const uint8_t *)data,
                     size) != FW_OK) {
        (void)s_backend->erase(s_backend->ctx, offset, s_backend->slot_size);
        return FW_EIO;
    }

    /* 读回槽头，确认序号/长度/CRC 已经落盘。 */
    if (s_backend->read(s_backend->ctx, offset, &readback,
                        sizeof(readback)) != FW_OK ||
        readback.magic != header.magic ||
        readback.version != header.version ||
        readback.sequence != header.sequence ||
        readback.size != header.size || readback.crc != header.crc) {
        (void)s_backend->erase(s_backend->ctx, offset, s_backend->slot_size);
        return FW_EIO;
    }

    s_active_slot = target;
    s_valid = true;
    ++s_next_sequence;
    if (s_next_sequence == 0U) s_next_sequence = 1U;
    return FW_OK;
}

fw_status_t fw_param_clear(void)
{
    int slot;
    fw_status_t first_error = FW_OK;

    if (s_backend == NULL || s_data_max == 0U) return FW_ENOTREADY;
    for (slot = 0; slot < 2; ++slot) {
        int result = s_backend->erase(s_backend->ctx, slot_offset(slot),
                                      s_backend->slot_size);
        if (result != FW_OK && first_error == FW_OK)
            first_error = (fw_status_t)result;
    }
    s_active_slot = -1;
    s_valid = false;
    s_next_sequence = 1U;
    return first_error;
}

size_t fw_param_capacity(void)
{
    return s_backend != NULL ? s_data_max : 0U;
}

bool fw_param_valid(void)
{
    return s_valid;
}
