/*
 * drv_serial.h - STM32 串口实例表驱动接口。
 *
 * 层级：src/platform/stm32f407
 * 职责：
 *   1. 接收板级串口描述符，按 FW_USING_UARTn 注册 serial1..N；
 *   2. 每个实例包含 HAL 句柄、USART 指针、IRQ 号、RT 事件和 ring 存储；
 *   3. 提供注册、启动、按实例查询和中断处理入口。
 *
 * 上下文：
 *   - register/start/instance 查询：任务或启动上下文；
 *   - irq_handler：USART 中断上下文，必须非阻塞。
 *
 * 依赖：fw/serial.h、rtthread.h、stm32f4xx_hal.h。
 *
 * 设计说明：
 *   引脚/AF/波特率/IRQ 优先级由板级 drv_serial_hw_t 表提供；
 *   驱动不包含 board_config.h。串口类只保存 ring 控制块，
 *   真正的存储区放在驱动结构体中。
 */
#ifndef DRV_SERIAL_H
#define DRV_SERIAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "fw/serial.h"
#include "drv_dma.h"
#include <rtthread.h>
#include "stm32f4xx_hal.h"

/*
 * DMA 块大小；由 Kconfig 的 FW_SERIAL_DMA_CHUNK 生成到 fw_config.h。
 * 未生成时给一个安全默认值，保证独立编译/测试也能通过。
 *
 * RX 缓冲 = 2 * CHUNK（DMA circular，半传输/全传输各完成一半）；
 * TX staging = CHUNK（每次 DMA 启动只搬一块）。
 */
#ifndef FW_SERIAL_DMA_CHUNK
#define FW_SERIAL_DMA_CHUNK 128
#endif

#define DRV_SERIAL_DMA_RX_BUFSZ (2U * FW_SERIAL_DMA_CHUNK)
#define DRV_SERIAL_DMA_TX_BUFSZ (FW_SERIAL_DMA_CHUNK)

/*
 * 板级串口描述符。
 *
 * instance - USART 编号 1..3；
 * tx/rx    - 端口 0=A..4=E 与引脚号，tx_af/rx_af 为复用功能号；
 * baud     - 初始波特率；
 * irq_priority - NVIC 抢占优先级。
 */
typedef struct {
    uint8_t instance;
    uint8_t tx_port;
    uint8_t tx_pin;
    uint8_t tx_af;
    uint8_t rx_port;
    uint8_t rx_pin;
    uint8_t rx_af;
    uint32_t baud;
    uint8_t irq_priority;
    /*
     * DMA 路由由 boardgen 从 chip manifest 生成，板级只负责选择是否启用。
     * controller/stream/channel 为 0xFF 表示未分配；enabled 为 false 时
     * 驱动完全走原来的 RXNE/TXE 中断路径。
     */
    drv_dma_route_t dma_rx;
    drv_dma_route_t dma_tx;
    bool dma_rx_enabled;
    bool dma_tx_enabled;
} drv_serial_hw_t;

/*
 * 单个串口实例。
 *
 * base       - 串口类对象，必须放在第一个成员；
 * hal        - HAL 串口句柄；
 * instance   - USARTx 寄存器块；
 * irq        - NVIC 中断号；
 * irq_priority - 板级配置的抢占优先级；
 * rx_event   - RT-Thread 事件；
 * started    - 中断是否已经打开；
 * tx_active  - TXE 中断是否已打开；
 * rx/tx_storage - 静态 ring 存储；
 * drv        - 驱动描述。
 */
typedef struct {
    fw_serial_t base;
    UART_HandleTypeDef hal;
    USART_TypeDef *instance;
    IRQn_Type irq;
    uint8_t irq_priority;
    struct rt_event rx_event;
    volatile bool started;
    volatile bool tx_active; /* IT 路径：TXE 是否已打开 */
    uint8_t rx_storage[FW_SERIAL_RX_BUFSZ];
    uint8_t tx_storage[FW_SERIAL_TX_BUFSZ];

    /*
     * DMA 状态。
     *
     * hdma_rx/hdma_tx 由本驱动持有并初始化；drv_dma.c 只记录 owner 和
     * 分发 IRQ。rx_dma_buf 是 circular DMA 的目标，必须位于主 SRAM；
     * tx_dma_buf 是 TX ring 的 staging 块，DMA 发送完成后再消费 ring。
     *
     * dma_rx_active/dma_tx_active 是“硬件正在搬运”的标志，供 configure、
     * flush 和错误恢复判断，不用于 ring 计数。
     */
    DMA_HandleTypeDef hdma_rx;
    DMA_HandleTypeDef hdma_tx;
    drv_dma_route_t dma_rx_route;
    drv_dma_route_t dma_tx_route;
    uint8_t rx_dma_buf[DRV_SERIAL_DMA_RX_BUFSZ];
    uint8_t tx_dma_buf[DRV_SERIAL_DMA_TX_BUFSZ];
    volatile bool dma_rx_active;
    volatile bool dma_tx_active;
    volatile uint16_t tx_stage_len; /* 当前 staging 块长度，完成回调消费用 */
    bool dma_rx_enabled;
    bool dma_tx_enabled;

    fw_drv_t drv;
} drv_serial_t;

/*
 * 按板级描述符注册串口。
 *
 * 对每个描述符：配置 TX/RX 引脚、使能 USART 时钟、HAL_UART_Init、
 * 初始化 RT 事件并注册 serialN。不打开 NVIC 中断。
 */
fw_status_t drv_serial_register_all(const drv_serial_hw_t *hw, size_t count);

/* 打开所有已注册串口的中断；重复调用安全。 */
fw_status_t drv_serial_start_all(void);

/* 按实例编号返回驱动对象；未启用返回 NULL。 */
drv_serial_t *drv_serial_instance(uint8_t instance);

/* 按实例编号返回设备基类；未启用返回 NULL。 */
fw_dev_t *drv_serial_dev(uint8_t instance);

/* 统一 USART 中断处理体；调用者必须已进入 RT-Thread 中断上下文。 */
void drv_serial_irq_handler(drv_serial_t *serial);

#endif
