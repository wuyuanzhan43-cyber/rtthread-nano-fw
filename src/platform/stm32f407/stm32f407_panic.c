/*
 * stm32f407_panic.c - STM32F407 致命错误现场实现。
 *
 * 层级：src/platform/stm32f407
 * 职责：关闭所有可屏蔽中断，保存文件/行号，原地空转。
 * 上下文：任意上下文。
 * 依赖：stm32f407_panic.h、CMSIS。
 */
#include "stm32f407_panic.h"
#include <stm32f4xx_hal.h>

volatile const char *g_stm32f407_panic_file;
volatile unsigned g_stm32f407_panic_line;

void stm32f407_panic(const char *file, unsigned line)
{
    __disable_irq();
    g_stm32f407_panic_file = file;
    g_stm32f407_panic_line = line;
    for (;;) __NOP();
}
