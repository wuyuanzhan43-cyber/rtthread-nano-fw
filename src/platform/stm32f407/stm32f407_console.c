/*
 * stm32f407_console.c - RT-Thread 控制台输出实现。
 *
 * 层级：src/platform/stm32f407
 * 职责：
 *   1. 提供强符号 rt_hw_console_output；
 *   2. 启动阶段轮询 USART，运行阶段写入 serial TX ring；
 *   3. 不依赖 board 头文件，只依赖板级绑定的串口实例。
 *
 * 上下文：
 *   - bind 在设备注册阶段调用；
 *   - 输出可能在任何任务/中断上下文调用，运行期必须非阻塞。
 *
 * 依赖：stm32f407_console.h、rtthread.h、string.h。
 */
#include "stm32f407_console.h"
#include <rtthread.h>
#include <string.h>

#define CONSOLE_TX_GUARD 1000000U

static drv_serial_t *s_console;

void stm32f407_console_bind(drv_serial_t *console)
{
    s_console = console;
}

/* 启动阶段的阻塞式发送；带 guard，避免外设异常时死等。 */
static void console_poll_write(drv_serial_t *console, const uint8_t *data,
                               size_t length)
{
    size_t i;

    for (i = 0U; i < length; ++i) {
        uint32_t guard = 0U;
        while ((console->instance->SR & USART_SR_TXE) == 0U) {
            if (++guard >= CONSOLE_TX_GUARD) return;
        }
        console->instance->DR = data[i];
    }
    {
        uint32_t guard = 0U;
        while ((console->instance->SR & USART_SR_TC) == 0U) {
            if (++guard >= CONSOLE_TX_GUARD) return;
        }
    }
}

void rt_hw_console_output(const char *str)
{
    drv_serial_t *console = s_console;
    size_t length;

    if (console == NULL || str == NULL) return;
    length = strlen(str);

    if (console->started) {
        (void)fw_dev_write(&console->base.dev, 0, (const uint8_t *)str,
                           length, FW_NO_WAIT);
    } else {
        console_poll_write(console, (const uint8_t *)str, length);
    }
}
