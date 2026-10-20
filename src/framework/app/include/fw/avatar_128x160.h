/*
 * fw/avatar_128x160.h - 128x160 RGB565 启动图片。
 *
 * 层级：src/framework/app/include/fw
 * 职责：声明启动图片数组，供 board_boot_display() 画到 ST7735S 上。
 * 上下文：只读常量，链接后位于 Flash。
 * 依赖：stdint.h。
 *
 * 存储格式：
 *   - 行优先，每行 128 个像素；
 *   - 每个元素是 RGB565（高 5 位红、中间 6 位绿、低 5 位蓝）；
 *   - 数组按 C 的 uint16_t 存放，面板驱动发送时会拆成
 *     大端字节序（高字节先发）。
 *
 * 空间占用：
 *   128 * 160 * 2 = 40960 字节（约 40KB）Flash。
 *   如果只需要纯色启动画面，可在 Kconfig 中选择白色诊断模式，
 *   从而把这张图片排除在构建之外（取决于链接器的 gc-sections）。
 */
#ifndef FW_AVATAR_128X160_H
#define FW_AVATAR_128X160_H

#include <stdint.h>

/* 图片逻辑尺寸，必须与 ST7735S 的 128x160 分辨率一致。 */
#define AVATAR_WIDTH 128U
#define AVATAR_HEIGHT 160U

/* 定义在 avatar_128x160.c 中；调用者只读，不要修改。 */
extern const uint16_t g_avatar_128x160[AVATAR_WIDTH * AVATAR_HEIGHT];

#endif
