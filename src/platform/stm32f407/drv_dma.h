/*
 * drv_dma.h - STM32F407 DMA stream 所有权与 IRQ 分发。
 *
 * 层级：src/platform/stm32f407
 * 职责：
 *   1. 为 UART/SPI 等外设分配并记录 DMA stream 的 owner；
 *   2. 使能 DMA 控制器时钟、NVIC 优先级和中断；
 *   3. 把 16 个 DMAx_Streamy_IRQHandler 统一分发到 HAL_DMA_IRQHandler；
 *   4. 拒绝把 CCM 地址交给 DMA（F407 的 DMA 不能访问 CCM）。
 *
 * 设计边界：
 *   - 本模块不实现 UART/SPI 的传输状态机，也不持有数据缓冲；
 *   - HAL_DMA_HandleTypeDef 由外设驱动持有并初始化；
 *   - 本模块只做“流所有权 + IRQ 入口”，避免复制 HAL 的 DMA 状态机。
 *
 * 上下文：
 *   - claim/release：初始化阶段（调度器启动前）或任务上下文；
 *   - irq_dispatch：DMA 中断上下文，必须短小、不可阻塞。
 *
 * 依赖：stm32f4xx_hal.h、fw/status.h、board_config.h（IRQ 优先级）。
 */
#ifndef DRV_DMA_H
#define DRV_DMA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "fw/status.h"
#include "stm32f4xx_hal.h"

/* 未分配/无效路由的哨兵值。 */
#define DRV_DMA_NONE 0xFFU

/*
 * 把板级 C 表里的 channel 编号 0..7 转成 HAL 的 DMA_CHANNEL_x。
 *
 * 注意：HAL 的 DMA_InitTypeDef.Channel 不是 0..7，而是 CR 寄存器位域宏
 * （DMA_CHANNEL_0..7）。直接把 4 写进去会选错请求，必须经过本映射。
 */
uint32_t drv_dma_channel_value(uint8_t channel);

/*
 * 一条 DMA 路由。
 *
 * controller - 1=DMA1, 2=DMA2；
 * stream     - 0..7；
 * channel    - 0..7（F4 的 DMA 请求按 channel 选择）。
 *
 * 路由在 board_config.h 的板级 C 表中手写，board.c 把它填进外设
 * 描述符；驱动不写死 stream/channel。
 */
typedef struct {
    uint8_t controller;
    uint8_t stream;
    uint8_t channel;
} drv_dma_route_t;

/*
 * 初始化 stream 所有权表。
 *
 * 当前实现只清零静态表；保留函数是为了以后加入 DMA2D/BDMA 等平台差异，
 * 也便于 Host 测试在用例之间复位。
 */
void drv_dma_init(void);

/*
 * 占用一条 DMA stream。
 *
 * 参数：
 *   handle - 外设驱动已经填好 Instance 的 HAL DMA 句柄；
 *   route  - controller/stream/channel 路由；
 *   owner  - 诊断字符串，例如 "serial1.rx"。
 *
 * 行为：
 *   1. 校验 route 和 controller/stream 范围；
 *   2. 如果同一 stream 已被占用，返回 FW_EBUSY（除非是同一个 handle）；
 *   3. 使能 DMA1/DMA2 时钟；
 *   4. 设置 NVIC 抢占优先级 BOARD_DMA_IRQ_PRIORITY 并使能 IRQ；
 *   5. 记录 handle，供 IRQ 分发使用。
 *
 * 注意：HAL_DMA_Init 必须在本函数之后调用，因为 HAL 需要 DMA 时钟已开。
 * 本函数不初始化 DMA 寄存器，也不启动传输。
 */
fw_status_t drv_dma_claim(DMA_HandleTypeDef *handle, const drv_dma_route_t *route,
                          const char *owner);

/*
 * 释放一条 stream。
 *
 * 只清 owner/handle 记录；不关闭 DMA 时钟，也不修改 NVIC。重复 release
 * 或传入未占用的 handle 是安全的 no-op。
 */
void drv_dma_release(DMA_HandleTypeDef *handle);

/*
 * 判断一段内存是否能安全用于 DMA。
 *
 * F407 的 CCM（0x10000000..0x1000FFFF）不能被 DMA 访问；这里显式拒绝，
 * 避免编译/链接都通过、上板才 HardFault。长度 0 或指针为空时返回 true
 * （调用者应该另行拒绝空缓冲）。
 */
bool drv_dma_ptr_ok(const void *ptr, size_t length);

/*
 * 由 16 个 DMAx_Streamy_IRQHandler 调用。
 *
 * 找不到 owner 时直接返回；正常情况下 NVIC 只在 claim 后才使能，
 * 因此不会出现未拥有流的中断风暴。找到 owner 后调用
 * HAL_DMA_IRQHandler(handle)，由 HAL 触发 HT/TC/TE 回调。
 *
 * 调用者必须已经 rt_interrupt_enter()。
 */
void drv_dma_irq_dispatch(uint8_t controller, uint8_t stream);

#endif
