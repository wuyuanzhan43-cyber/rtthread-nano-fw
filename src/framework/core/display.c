/*
 * display.c - display 设备类的通用实现。
 *
 * 层级：src/framework/core
 * 职责：
 *   1. 注册 display 设备并保存面板 ops/info；
 *   2. 实现矩形裁剪、分块填充和逐行位图发送；
 *   3. 通过 ioctl 转发背光控制。
 *
 * 上下文：任务上下文，允许阻塞。
 * 依赖：fw/display.h。
 *
 * 设计说明：
 *   类层不包含任何面板或 HAL 头文件；颜色格式固定为 RGB565。
 *   裁剪和分块放在这里，面板驱动只负责“设置窗口 + 写像素”，
 *   这样换面板时不需要重复实现边界处理。
 */
#include "fw/display.h"

/* 填充时每块发送的像素数：16*2=32 字节，适合栈上临时数组。 */
#define FW_DISPLAY_FILL_CHUNK 16U

/*
 * display 的驱动描述。
 *
 * display 与 pin/serial/spi 不同：它的具体操作放在 fw_display_t->ops，
 * 而不是 drv->ops。这里提供一个类型正确、ops 为空的 drv，
 * 只为满足 fw_dev_register 的统一接口。
 */
static const fw_drv_t display_drv = {
    FW_DEV_TYPE_DISPLAY, NULL, NULL
};

/*
 * display 唯一支持的 ioctl：背光开关。
 * 参数是 bool*；转发到面板驱动的 set_backlight。
 */
static int display_ioctl(fw_dev_t *dev, int off, int cmd, void *args)
{
    fw_display_t *display = FW_CONTAINER_OF(dev, fw_display_t, dev);
    (void)off;

    if (cmd == FW_CTL_DISPLAY_SET_BACKLIGHT) {
        bool on;
        if (args == NULL || display->ops == NULL ||
            display->ops->set_backlight == NULL) return FW_EINVAL;
        on = *(const bool *)args;
        return display->ops->set_backlight(display->ctx, on) == FW_OK
                   ? (int)sizeof(on) : FW_EIO;
    }
    return FW_ENOTSUP;
}

/* display 类操作表：只有 ioctl，具体绘图通过类辅助函数完成。 */
static const fw_dev_ops_t display_dev_ops = {
    NULL,
    NULL,
    NULL,
    NULL,
    display_ioctl,
    NULL
};

/*
 * 注册 display 设备。
 *
 * 校验：
 *   - 对象、名字、ops、info 非空；
 *   - 宽高大于 0；
 *   - 当前只支持 16bpp（RGB565）。
 * 注册成功后保存 ops/ctx/info，供后续绘图函数使用。
 */
fw_status_t fw_display_register(fw_display_t *display, const char *name,
                                const fw_display_ops_t *ops, void *ctx,
                                const fw_display_info_t *info)
{
    fw_status_t result;
    if (display == NULL || name == NULL || ops == NULL || info == NULL)
        return FW_EINVAL;
    if (info->width == 0U || info->height == 0U || info->bits_per_pixel != 16U)
        return FW_EINVAL;

    result = fw_dev_register(&display->dev, name, FW_DEV_TYPE_DISPLAY,
                             &display_drv);
    if (result != FW_OK) return result;

    display->dev.ops = &display_dev_ops;
    display->ops = ops;
    display->ctx = ctx;
    display->info = *info;
    return FW_OK;
}

/* 查找并做类型校验。 */
fw_display_t *fw_display_find(const char *name)
{
    fw_dev_t *dev = fw_dev_find(name);
    if (dev == NULL || dev->type != FW_DEV_TYPE_DISPLAY) return NULL;
    return FW_CONTAINER_OF(dev, fw_display_t, dev);
}

/* 调用面板驱动的初始化序列；驱动未实现时返回 FW_EINVAL。 */
fw_status_t fw_display_setup(fw_display_t *display)
{
    if (display == NULL || display->ops == NULL ||
        display->ops->init == NULL) return FW_EINVAL;
    return display->ops->init(display->ctx);
}

/*
 * 把矩形裁剪到屏幕范围。
 *
 * 使用 32 位中间变量计算右下角，避免 uint16_t 加法溢出：
 *   例如 x=65000、width=1000 时，16 位加法会回绕。
 *
 * 裁剪后 x/y 可能等于宽高，此时 width/height 会变成 0，
 * 调用者据此判断矩形完全在屏幕外。
 */
static void clip_rect(const fw_display_t *display, uint16_t *x, uint16_t *y,
                      uint16_t *width, uint16_t *height)
{
    uint32_t x0 = *x;
    uint32_t y0 = *y;
    uint32_t x1 = x0 + *width;
    uint32_t y1 = y0 + *height;

    if (x0 > display->info.width) x0 = display->info.width;
    if (y0 > display->info.height) y0 = display->info.height;
    if (x1 > display->info.width) x1 = display->info.width;
    if (y1 > display->info.height) y1 = display->info.height;

    *x = (uint16_t)x0;
    *y = (uint16_t)y0;
    *width = (uint16_t)(x1 - x0);
    *height = (uint16_t)(y1 - y0);
}

/*
 * 填充矩形。
 *
 * 步骤：
 *   1. 检查面板 ops 是否完整；
 *   2. 裁剪矩形，空矩形直接成功返回；
 *   3. 准备一个 16 像素的重复颜色块；
 *   4. begin_write 设置窗口；
 *   5. 循环发送颜色块，直到填满 width*height；
 *   6. end_write 收尾。
 *
 * 分块而不是一次性构造整个矩形，是为了避免在栈上分配
 * 最大 128*160*2=40KB 的缓冲；16 像素只需 32 字节。
 *
 * 错误处理：如果中途 write_pixels 失败，仍然调用 end_write，
 * 让面板退出写模式；最终返回第一个错误。
 */
fw_status_t fw_display_fill_rect(fw_display_t *display, uint16_t x, uint16_t y,
                                 uint16_t width, uint16_t height, uint16_t color,
                                 uint32_t timeout_ms)
{
    uint16_t chunk[FW_DISPLAY_FILL_CHUNK];
    uint32_t remaining;
    fw_status_t result;
    size_t i;

    if (display == NULL || display->ops == NULL ||
        display->ops->begin_write == NULL ||
        display->ops->write_pixels == NULL ||
        display->ops->end_write == NULL) return FW_EINVAL;

    clip_rect(display, &x, &y, &width, &height);
    if (width == 0U || height == 0U) return FW_OK;

    /* 预填充颜色块，后续每轮复用，避免重复赋值。 */
    for (i = 0U; i < FW_DISPLAY_FILL_CHUNK; ++i) chunk[i] = color;

    result = display->ops->begin_write(display->ctx, x, y, width, height,
                                       timeout_ms);
    if (result != FW_OK) return result;

    remaining = (uint32_t)width * height;
    while (remaining != 0U && result == FW_OK) {
        size_t count = remaining > FW_DISPLAY_FILL_CHUNK
                           ? FW_DISPLAY_FILL_CHUNK : (size_t)remaining;
        result = display->ops->write_pixels(display->ctx, chunk, count,
                                            timeout_ms);
        remaining -= (uint32_t)count;
    }

    /* 无论填充是否成功，都要结束窗口写入，保持面板状态一致。 */
    {
        fw_status_t end_result = display->ops->end_write(display->ctx);
        if (result == FW_OK) result = end_result;
    }
    return result;
}

/* 用单色填满整个屏幕。 */
fw_status_t fw_display_fill(fw_display_t *display, uint16_t color,
                            uint32_t timeout_ms)
{
    if (display == NULL) return FW_EINVAL;
    return fw_display_fill_rect(display, 0U, 0U, display->info.width,
                                display->info.height, color, timeout_ms);
}

/*
 * 绘制位图。
 *
 * 输入像素是行优先排列，stride 固定等于传入的 width。
 * 裁剪时只改变可见区域，源数据索引仍然使用原始 stride，
 * 因此裁剪后不会错位。
 *
 * 实现按行发送：每一行是一段连续 RGB565 数据，
 * 面板驱动只需要处理“窗口 + 像素流”，不需要知道位图布局。
 */
fw_status_t fw_display_draw_bitmap(fw_display_t *display, uint16_t x, uint16_t y,
                                   uint16_t width, uint16_t height,
                                   const uint16_t *pixels, uint32_t timeout_ms)
{
    uint16_t row;
    uint16_t stride;
    fw_status_t result;

    if (display == NULL || pixels == NULL || display->ops == NULL ||
        display->ops->begin_write == NULL ||
        display->ops->write_pixels == NULL ||
        display->ops->end_write == NULL) return FW_EINVAL;

    /* stride 必须在裁剪前保存，裁剪只影响可见区域。 */
    stride = width;
    clip_rect(display, &x, &y, &width, &height);
    if (width == 0U || height == 0U) return FW_OK;

    result = display->ops->begin_write(display->ctx, x, y, width, height,
                                       timeout_ms);
    if (result != FW_OK) return result;

    for (row = 0U; row < height && result == FW_OK; ++row) {
        const uint16_t *source = pixels + ((size_t)y + row) * stride + x;
        result = display->ops->write_pixels(display->ctx, source, width,
                                            timeout_ms);
    }

    /* 与填充相同：即使中途失败也结束写窗口，再返回第一个错误。 */
    {
        fw_status_t end_result = display->ops->end_write(display->ctx);
        if (result == FW_OK) result = end_result;
    }
    return result;
}

/* 背光控制便捷函数，直接调用面板驱动。 */
fw_status_t fw_display_set_backlight(fw_display_t *display, bool on)
{
    if (display == NULL || display->ops == NULL ||
        display->ops->set_backlight == NULL) return FW_EINVAL;
    return display->ops->set_backlight(display->ctx, on);
}
