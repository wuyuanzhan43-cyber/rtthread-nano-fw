/*
 * test_param.c - fw_param 双槽参数存储 Host 测试。
 *
 * 层级：src/tests/host
 * 职责：
 *   1. 用 RAM 假 Flash 验证 save/load/clear；
 *   2. 验证 A/B 槽交替、CRC 损坏回退、容量检查；
 *   3. 不模拟真实 Flash 擦除时序，只验证存储策略。
 *
 * 上下文：单线程。
 * 依赖：test_common.h、fw/param.h、string.h。
 */
#include "test_common.h"
#include "fw/param.h"

#define FAKE_SLOT_SIZE 256U
#define FAKE_DATA_MAX 128U
#define FAKE_TOTAL (FAKE_SLOT_SIZE * 2U)

static uint8_t s_flash[FAKE_TOTAL];

static void fake_reset(void)
{
    memset(s_flash, 0xFF, sizeof(s_flash));
}

static int fake_read(void *ctx, uint32_t offset, void *buf, size_t len)
{
    (void)ctx;
    if (offset > sizeof(s_flash) || len > sizeof(s_flash) - offset)
        return FW_EINVAL;
    memcpy(buf, &s_flash[offset], len);
    return FW_OK;
}

static int fake_program(void *ctx, uint32_t offset, const void *buf,
                        size_t len)
{
    (void)ctx;
    if (offset > sizeof(s_flash) || len > sizeof(s_flash) - offset ||
        offset % 4U != 0U || len % 4U != 0U)
        return FW_EINVAL;
    memcpy(&s_flash[offset], buf, len);
    return FW_OK;
}

static int fake_erase(void *ctx, uint32_t offset, size_t len)
{
    (void)ctx;
    if (offset > sizeof(s_flash) || len > sizeof(s_flash) - offset)
        return FW_EINVAL;
    memset(&s_flash[offset], 0xFF, len);
    return FW_OK;
}

static const fw_param_backend_t s_backend = {
    fake_read,
    fake_program,
    fake_erase,
    FAKE_SLOT_SIZE,
    NULL
};

static void test_param_basic(void)
{
    uint8_t data[FAKE_DATA_MAX];
    uint8_t readback[FAKE_DATA_MAX];
    size_t size = 0U;

    fake_reset();
    CHECK(fw_param_init(&s_backend, FAKE_DATA_MAX) == FW_OK);
    CHECK(!fw_param_valid());
    CHECK(fw_param_load(readback, sizeof(readback), &size) == FW_ENODATA);

    memset(data, 0x5A, sizeof(data));
    CHECK(fw_param_save(data, 16U) == FW_OK);
    CHECK(fw_param_valid());
    CHECK(fw_param_load(readback, sizeof(readback), &size) == FW_OK);
    CHECK(size == 16U && memcmp(readback, data, 16U) == 0);

    /* 第二次保存必须写到另一个槽，并保留旧槽内容。 */
    memset(data, 0xA5, sizeof(data));
    CHECK(fw_param_save(data, 32U) == FW_OK);
    CHECK(fw_param_load(readback, sizeof(readback), &size) == FW_OK);
    CHECK(size == 32U && memcmp(readback, data, 32U) == 0);

    /* 缓冲区不足：返回 EOVERFLOW，同时报告所需大小。 */
    CHECK(fw_param_load(readback, 8U, &size) == FW_EOVERFLOW);
    CHECK(size == 32U);

    /* 参数容量与非法长度。 */
    CHECK(fw_param_capacity() == FAKE_DATA_MAX);
    CHECK(fw_param_save(data, FAKE_DATA_MAX + 1U) == FW_EINVAL);

    CHECK(fw_param_clear() == FW_OK);
    CHECK(!fw_param_valid());
    CHECK(fw_param_load(readback, sizeof(readback), &size) == FW_ENODATA);
}

static void test_param_crc_fallback(void)
{
    uint8_t data[FAKE_DATA_MAX];
    uint8_t readback[FAKE_DATA_MAX];
    size_t size = 0U;
    uint32_t active_offset;

    fake_reset();
    CHECK(fw_param_init(&s_backend, FAKE_DATA_MAX) == FW_OK);

    memset(data, 0x11, sizeof(data));
    CHECK(fw_param_save(data, 8U) == FW_OK);
    memset(data, 0x22, sizeof(data));
    CHECK(fw_param_save(data, 8U) == FW_OK);

    /*
     * 第二次保存写到 slot 1；破坏其数据区的第一个字节，再重新 init，
     * 应回退到 slot 0 的 0x11。
     */
    active_offset = FAKE_SLOT_SIZE + FW_PARAM_HEADER_SIZE;
    s_flash[active_offset] ^= 0xFFU;

    CHECK(fw_param_init(&s_backend, FAKE_DATA_MAX) == FW_OK);
    CHECK(fw_param_valid());
    CHECK(fw_param_load(readback, sizeof(readback), &size) == FW_OK);
    CHECK(size == 8U && readback[0] == 0x11U);

    /* 再破坏另一个槽，两个槽都无效。 */
    s_flash[FW_PARAM_HEADER_SIZE] ^= 0xFFU;
    CHECK(fw_param_init(&s_backend, FAKE_DATA_MAX) == FW_OK);
    CHECK(!fw_param_valid());
    CHECK(fw_param_load(readback, sizeof(readback), &size) == FW_ENODATA);
}

static void test_param_invalid_backend(void)
{
    CHECK(fw_param_init(NULL, FAKE_DATA_MAX) == FW_EINVAL);
    {
        fw_param_backend_t bad = s_backend;
        bad.slot_size = FW_PARAM_HEADER_SIZE;
        CHECK(fw_param_init(&bad, FAKE_DATA_MAX) == FW_EINVAL);
    }
}

void test_param_run(void)
{
    test_param_basic();
    test_param_crc_fallback();
    test_param_invalid_backend();
}
