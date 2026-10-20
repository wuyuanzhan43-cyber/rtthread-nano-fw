/*
 * drv_serial.h - STM32 串口实例表驱动接口。
 *
 * 层级：src/bsp/st/driver
 * 职责：
 *   1. 按 Kconfig 的 FW_USING_UARTn 生成 serial1..N；
 *   2. 每个实例包含 HAL 句柄、USART 指针、IRQ 号、RT 事件和 ring 存储；
 *   3. 提供注册、启动、控制台查询和中断处理入口。
 *
 * 上下文：
 *   - init/start/console 查询：任务或启动上下文；
 *   - irq_handler：USART 中断上下文，必须非阻塞。
 *
 * 依赖：fw/serial.h、rtthread.h、stm32f4xx_hal.h、board_config.h。
 *
 * 设计说明：
 *   串口类（fw_serial_t）只保存 ring 控制块，真正的存储区放在
 *   驱动结构体中，这样容量可以由 Kconfig 的 FW_SERIAL_RX_BUFSZ
 *   和 FW_SERIAL_TX_BUFSZ 决定，而类层保持芯片无关。
 */
#ifndef DRV_SERIAL_H
#define DRV_SERIAL_H

#include <stdbool.h>
#include <stdint.h>
#include "fw/serial.h"
#include <rtthread.h>
#include "stm32f4xx_hal.h"

/*
 * 单个串口实例。
 *
 * base       - 串口类对象，必须放在第一个成员，注册时把 &base.dev
 *              放进设备表；
 * hal        - HAL 串口句柄，保存寄存器基址和初始化参数；
 * instance   - 指向 USARTx 寄存器块，ISR 中直接读写 SR/DR；
 * irq        - NVIC 中断号，用于设置优先级和使能；
 * rx_event   - RT-Thread 事件对象，ISR 收到数据后唤醒等待任务；
 * started    - 中断是否已经打开（board_console_start 之后为 true）；
 * tx_active  - TXE 中断是否处于打开状态，避免重复使能；
 * rx_storage - 接收 ring 的静态存储，大小来自 Kconfig；
 * tx_storage - 发送 ring 的静态存储，大小来自 Kconfig；
 * drv        - 驱动描述，ops 指向本文件的 fw_serial_ops_t。
 *
 * 整个结构体都是静态分配；一个实例的 RAM 占用约为
 * sizeof(结构体) + RX_BUFSZ + TX_BUFSZ。
 */
typedef struct {
    fw_serial_t base;
    UART_HandleTypeDef hal;
    USART_TypeDef *instance;
    IRQn_Type irq;
    struct rt_event rx_event;
    volatile bool started;
    volatile bool tx_active;
    uint8_t rx_storage[FW_SERIAL_RX_BUFSZ];
    uint8_t tx_storage[FW_SERIAL_TX_BUFSZ];
    fw_drv_t drv;
} drv_serial_t;

/*
 * 注册所有已启用的串口。
 *
 * 对每个 FW_USING_UARTn=y 的实例：
 *   - 配置 TX/RX 引脚复用；
 *   - 使能 USART 时钟并调用 HAL_UART_Init；
 *   - 初始化 RT 事件；
 *   - 注册 serialN 设备。
 *
 * 此函数不打开 NVIC 中断；中断在 drv_serial_start_all() 中统一打开，
 * 保证 ring/设备已经准备好。
 */
fw_status_t drv_serial_register_all(void);

/*
 * 打开所有已注册串口的中断。
 *
 * 设置 NVIC 优先级、使能 RXNE/ERR 中断，并把 started 置 true。
 * 必须在注册完成之后调用；重复调用是安全的。
 */
fw_status_t drv_serial_start_all(void);

/* 返回控制台对应的驱动实例；没有匹配的 UART 时返回 NULL。 */
drv_serial_t *drv_serial_console(void);

/* 返回控制台对应的设备基类指针，供 fw_dev_open/read/write 使用。 */
fw_dev_t *drv_serial_console_dev(void);

/*
 * 统一的中断处理体。
 *
 * 由各 USARTx_IRQHandler 调用；内部根据 SR 判断 RXNE/TXE/错误，
 * 并转发给 serial 类。调用者必须已经进入 RT-Thread 中断上下文。
 */
void drv_serial_irq_handler(drv_serial_t *serial);

#endif
