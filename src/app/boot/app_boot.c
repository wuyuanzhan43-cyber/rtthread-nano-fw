/*
 * app_boot.c - 应用层启动画面：色条、头像或纯白诊断。
 *
 * 层级：src/app/boot
 * 职责：
 *   1. 调用 fw_display_setup 完成面板初始化；
 *   2. 根据 APP_BOOT_MODE 选择启动画面；
 *   3. 不依赖 board、HAL 或 RT-Thread。
 *
 * 上下文：main 线程；SPI 传输可能阻塞。
 * 依赖：app/boot.h、app_config.h、fw/display.h；
 *       APP_BOOT_MODE=1 时额外依赖 avatar_128x160.h。
 *
 * 为什么用编译期分支：
 *   头像数组约 40KB。若用运行时 switch，即使选择色条模式，
 *   链接器仍会保留头像数据；#if 可以让未选中模式完全不引用它，
 *   再由 --gc-sections 回收。
 */
#include "app/boot.h"
#include "app_config.h"

#if defined(APP_BOOT_DISPLAY) && APP_BOOT_DISPLAY && \
    defined(APP_BOOT_MODE) && (APP_BOOT_MODE == 1)
#include "../assets/avatar_128x160.h"
#endif

/* 启动画面单次填充/位图传输的超时时间。 */
#define APP_BOOT_TIMEOUT_MS 100U

/* RGB565 基本颜色；色条模式使用。 */
#define APP_BOOT_COLOR_BLACK 0x0000U
#define APP_BOOT_COLOR_RED   0xF800U
#define APP_BOOT_COLOR_GREEN 0x07E0U
#define APP_BOOT_COLOR_BLUE  0x001FU
#define APP_BOOT_COLOR_WHITE 0xFFFFU

#if defined(APP_BOOT_DISPLAY) && APP_BOOT_DISPLAY && \
    defined(APP_BOOT_MODE) && (APP_BOOT_MODE == 0)
/*
 * 四色横条：把屏幕高度四等分，依次填红、绿、蓝、白。
 * 用于检查 SPI、窗口设置、颜色顺序和面板偏移。
 */
static fw_status_t draw_color_bars(fw_display_t *display)
{
    uint16_t bar = (uint16_t)(display->info.height / 4U);
    fw_status_t result;

    if (bar == 0U) return FW_EINVAL;

    result = fw_display_fill(display, APP_BOOT_COLOR_BLACK,
                             APP_BOOT_TIMEOUT_MS);
    if (result != FW_OK) return result;

    result = fw_display_fill_rect(display, 0U, 0U, display->info.width, bar,
                                  APP_BOOT_COLOR_RED, APP_BOOT_TIMEOUT_MS);
    if (result != FW_OK) return result;

    result = fw_display_fill_rect(display, 0U, bar, display->info.width, bar,
                                  APP_BOOT_COLOR_GREEN, APP_BOOT_TIMEOUT_MS);
    if (result != FW_OK) return result;

    result = fw_display_fill_rect(display, 0U, (uint16_t)(bar * 2U),
                                  display->info.width, bar,
                                  APP_BOOT_COLOR_BLUE, APP_BOOT_TIMEOUT_MS);
    if (result != FW_OK) return result;

    return fw_display_fill_rect(display, 0U, (uint16_t)(bar * 3U),
                                display->info.width,
                                (uint16_t)(display->info.height - bar * 3U),
                                APP_BOOT_COLOR_WHITE, APP_BOOT_TIMEOUT_MS);
}
#endif

fw_status_t app_boot_display(fw_display_t *display)
{
#if defined(APP_BOOT_DISPLAY) && APP_BOOT_DISPLAY
    fw_status_t result;

    if (display == NULL) return FW_ENOTSUP;

    result = fw_display_setup(display);
    if (result != FW_OK) return result;

#if defined(APP_BOOT_MODE) && (APP_BOOT_MODE == 0)
    return draw_color_bars(display);
#elif defined(APP_BOOT_MODE) && (APP_BOOT_MODE == 2)
    return fw_display_fill(display, APP_BOOT_COLOR_WHITE,
                           APP_BOOT_TIMEOUT_MS);
#else
    return fw_display_draw_bitmap(display, 0U, 0U, AVATAR_WIDTH, AVATAR_HEIGHT,
                                  g_avatar_128x160, APP_BOOT_TIMEOUT_MS);
#endif
#else
    (void)display;
    return FW_ENOTSUP;
#endif
}
