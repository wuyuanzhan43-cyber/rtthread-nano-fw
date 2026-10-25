/*
 * stm32f407_panic.c - STM32F407 致命错误现场实现。
 *
 * 层级：src/platform/stm32f407
 * 职责：关闭中断、保存文件/行号、直写控制台，然后按板级策略复位或停机。
 * 上下文：任意上下文；只使用轮询控制台和寄存器访问。
 * 依赖：stm32f407_panic.h、stm32f407_console.h、board_config.h、CMSIS。
 */
#include "stm32f407_panic.h"
#include "stm32f407_console.h"
#include "board_config.h"
#include <stm32f4xx_hal.h>

volatile const char *g_stm32f407_panic_file;
volatile unsigned g_stm32f407_panic_line;

static void panic_puts(const char *text)
{
    stm32f407_console_panic_write(text);
}

static void panic_put_unsigned(unsigned value)
{
    char buffer[11];
    int index = 0;

    if (value == 0U) {
        panic_puts("0");
        return;
    }
    while (value != 0U && index < (int)sizeof(buffer) - 1) {
        buffer[index++] = (char)('0' + (value % 10U));
        value /= 10U;
    }
    buffer[index] = '\0';
    while (index > 0) {
        char one[2];
        one[0] = buffer[--index];
        one[1] = '\0';
        panic_puts(one);
    }
}

void stm32f407_panic(const char *file, unsigned line)
{
    __disable_irq();
    g_stm32f407_panic_file = file;
    g_stm32f407_panic_line = line;

    panic_puts("*** PANIC at ");
    panic_puts(file != NULL ? file : "?");
    panic_puts(":");
    panic_put_unsigned(line);
    panic_puts("\n");

#if defined(BOARD_FAULT_RESET) && BOARD_FAULT_RESET
    panic_puts("*** resetting\n");
    NVIC_SystemReset();
#endif

    panic_puts("*** halted\n");
    for (;;) {
        __NOP();
    }
}
