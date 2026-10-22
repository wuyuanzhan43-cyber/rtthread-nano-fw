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

    if (!console->started) {
        /*
         * 调度器启动前 UART 中断尚未使能，只能轮询 DR。
         * 这条路径也用于 board_devices_init 之前的启动诊断。
         */
        console_poll_write(console, (const uint8_t *)str, length);
        return;
    }

    /*
     * 运行期：
     *   - 任务上下文：正常写 ring；IT/DMA 路径由驱动决定；
     *   - 中断上下文 + IT 路径：写 ring 并打开 TXE，允许非阻塞；
     *   - 中断上下文 + DMA 路径：HAL DMA API 不适合从任意 ISR 启动，
     *     而且直接轮询 DR 会和 DMA 抢数据。这里选择丢弃该行诊断，
     *     避免在中断里引入不可预测的 HAL 状态机。
     */
    if (rt_interrupt_get_nest() == 0U) {
        (void)fw_dev_write(&console->base.dev, 0, (const uint8_t *)str,
                           length, FW_NO_WAIT);
    } else if (!console->dma_tx_enabled) {
        (void)fw_dev_write(&console->base.dev, 0, (const uint8_t *)str,
                           length, FW_NO_WAIT);
    }
}
