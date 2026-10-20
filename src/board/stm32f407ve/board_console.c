/*
 * board_console.c - RT-Thread 控制台输出适配。
 *
 * 层级：src/board/stm32f407ve
 * 职责：
 *   1. 在没有 RT-Thread device framework 的情况下提供强符号
 *      rt_hw_console_output；
 *   2. 启动阶段（调度器/串口中断尚未就绪）使用轮询发送；
 *   3. 运行阶段把字符串放入 serial TX ring，非阻塞返回。
 *
 * 上下文：
 *   - 启动 banner 在调度器启动前输出，此时只能轮询；
 *   - 运行期 rt_kprintf 可能在任何任务上下文调用，入队即可。
 *
 * 依赖：board.h、drv_serial.h、rtthread.h、string.h。
 *
 * 为什么不直接使用 RT-Thread 的 device framework：
 *   本项目只使用 Nano 内核，没有设备框架；rt_kprintf 需要一个
 *   rt_hw_console_output 实现。这里用最小代码把它接到自己的
 *   serial 类上，既能输出启动日志，也不引入额外组件。
 *
 * 两种模式：
 *
 *   未启动（started=false）:
 *     直接轮询 USART SR/DR，逐字节发送，最后等待 TC。
 *     此时中断未开，不能用 ring（没人消费）。
 *
 *   已启动（started=true）:
 *     fw_dev_write 把数据放进 TX ring，TXE 中断负责实际发送。
 *     rt_kprintf 不等待发送完成，避免在日志很多时阻塞业务。
 *     如果 ring 满，serial_write 会短写，多余字符被丢弃；
 *     对调试日志来说这是可接受的取舍。
 */
#include "board.h"
#include <rtthread.h>
#include <string.h>

/*
 * 轮询发送的循环上限。
 *
 * 正常 USART 发送一个字节只需要几十微秒；如果 TXE/TC 一直不置位，
 * 说明外设时钟或引脚配置有问题。这里设置一个足够大的上限，
 * 避免启动阶段永远卡死在控制台输出里（那样连 panic 信息都看不到）。
 */
#define CONSOLE_TX_GUARD 1000000U

/*
 * 启动阶段的阻塞式发送。
 *
 * 逐字节等待 TXE，写 DR；全部写完后等待 TC，确保最后一个字节
 * 已经移出移位寄存器（否则复位/掉电时最后几个字符可能丢失）。
 *
 * 每个等待都有 guard 上限；超时直接返回，宁可丢字符也不死等。
 */
static void poll_write(drv_serial_t *console, const uint8_t *data, size_t length)
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

/*
 * RT-Thread 控制台弱符号的强实现。
 *
 * - str 由 rt_kprintf 传入，保证以 '\0' 结尾；
 * - 控制台未注册或尚未初始化时静默返回，避免启动早期崩溃；
 * - 已启动后使用非阻塞入队；未启动则轮询。
 *
 * 注意：rt_hw_console_output 可能在中断上下文中被调用（例如断言），
 * 因此运行期路径必须是非阻塞的；fw_dev_write 只操作 ring 和
 * 开 TXE 中断，满足要求。
 */
void rt_hw_console_output(const char *str)
{
    drv_serial_t *console = board_console_driver();
    size_t length;
    if (console == NULL || str == NULL) return;
    length = strlen(str);

    if (console->started) {
        fw_dev_t *dev = &console->base.dev;
        (void)fw_dev_write(dev, 0, (const uint8_t *)str, length, FW_NO_WAIT);
    } else {
        poll_write(console, (const uint8_t *)str, length);
    }
}
