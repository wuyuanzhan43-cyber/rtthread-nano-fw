/*
 * drv_dma.c - STM32F407 DMA stream 所有权表与 IRQ 入口。
 *
 * 层级：src/platform/stm32f407
 * 职责：
 *   1. 维护 controller/stream -> DMA_HandleTypeDef* 的静态映射；
 *   2. claim 时打开 DMA 控制器时钟和对应 NVIC 中断；
 *   3. 16 个 DMA stream 中断统一转给 HAL_DMA_IRQHandler；
 *   4. 提供 CCM 地址检查，阻止不可 DMA 的缓冲进入硬件。
 *
 * 为什么需要 owner 表：
 *   HAL 的 DMA 中断入口是 DMAx_Streamy_IRQHandler，而每个 stream 的
 *   HAL_DMA_HandleTypeDef 由不同外设持有（huart->hdmarx、hspi->hdmatx...）。
 *   没有 owner 表时，中断入口无法知道该调用哪个 HAL 句柄。这里用
 *   (controller-1)*8+stream 作为固定下标，避免动态查找和 malloc。
 *
 * 并发：
 *   claim/release 在调度器启动前完成；IRQ 只在 claim 后使能。
 *   dispatch 只读 owner 表，不做加锁；表项在运行期不再修改。
 *
 * 依赖：drv_dma.h、stm32f407_map.h、board_config.h、rthw.h、string.h。
 */
#include "drv_dma.h"
#include "stm32f407_map.h"
#include "board_config.h"
#include <rthw.h>
#include <string.h>

/* DMA1/DMA2 各 8 个 stream，共 16 个槽位。 */
#define DRV_DMA_SLOT_COUNT 16U

typedef struct {
    DMA_HandleTypeDef *handle; /* NULL 表示 stream 空闲 */
    const char *owner;         /* 诊断用，例如 "serial1.rx" */
} drv_dma_slot_t;

static drv_dma_slot_t s_dma_slots[DRV_DMA_SLOT_COUNT];

uint32_t drv_dma_channel_value(uint8_t channel)
{
    switch (channel) {
    case 0U: return DMA_CHANNEL_0;
    case 1U: return DMA_CHANNEL_1;
    case 2U: return DMA_CHANNEL_2;
    case 3U: return DMA_CHANNEL_3;
    case 4U: return DMA_CHANNEL_4;
    case 5U: return DMA_CHANNEL_5;
    case 6U: return DMA_CHANNEL_6;
    case 7U: return DMA_CHANNEL_7;
    default: return 0xFFFFFFFFU;
    }
}

/* controller/stream 组合是否合法。 */
static bool route_valid(const drv_dma_route_t *route)
{
    return route != NULL &&
           (route->controller == 1U || route->controller == 2U) &&
           route->stream <= 7U && route->channel <= 7U;
}

/* controller/stream 转静态槽位下标；非法组合返回 DRV_DMA_SLOT_COUNT。 */
static size_t slot_index(const drv_dma_route_t *route)
{
    if (!route_valid(route)) return DRV_DMA_SLOT_COUNT;
    return (size_t)(route->controller - 1U) * 8U + route->stream;
}

void drv_dma_init(void)
{
    memset(s_dma_slots, 0, sizeof(s_dma_slots));
}

/*
 * 占用一条 DMA stream。
 *
 * 顺序说明：
 *   1. 先校验路由，避免非法 controller/stream 计算越界下标；
 *   2. 检查是否已被同一个或另一个 handle 占用；
 *   3. 打开控制器时钟（HAL_DMA_Init 需要）；
 *   4. 配置 NVIC，使能 stream 中断；
 *   5. 最后写入 owner 表。
 *
 * 失败路径不会留下“NVIC 已开但 owner 为空”的状态：所有校验都在
 * 打开时钟/NVIC 之前完成；如果 HAL 句柄的 Instance 与路由不一致，
 * 也在修改硬件之前返回。
 */
fw_status_t drv_dma_claim(DMA_HandleTypeDef *handle, const drv_dma_route_t *route,
                          const char *owner)
{
    size_t index;
    IRQn_Type irq;

    if (handle == NULL || !route_valid(route)) return FW_EINVAL;
    if (handle->Instance != stm32f407_dma_stream(route->controller, route->stream))
        return FW_EINVAL;

    index = slot_index(route);
    if (index >= DRV_DMA_SLOT_COUNT) return FW_EINVAL;
    if (s_dma_slots[index].handle != NULL) {
        /* 同一 handle 重复 claim 是幂等的；不同 owner 视为冲突。 */
        return s_dma_slots[index].handle == handle ? FW_OK : FW_EBUSY;
    }

    if (route->controller == 1U) __HAL_RCC_DMA1_CLK_ENABLE();
    else __HAL_RCC_DMA2_CLK_ENABLE();

    irq = stm32f407_dma_irq(route->controller, route->stream);
    if ((int)irq < 0) return FW_EINVAL;
    HAL_NVIC_SetPriority(irq, BOARD_DMA_IRQ_PRIORITY, 0U);
    HAL_NVIC_EnableIRQ(irq);

    s_dma_slots[index].handle = handle;
    s_dma_slots[index].owner = owner;
    return FW_OK;
}

void drv_dma_release(DMA_HandleTypeDef *handle)
{
    size_t i;

    if (handle == NULL) return;
    for (i = 0U; i < DRV_DMA_SLOT_COUNT; ++i) {
        if (s_dma_slots[i].handle == handle) {
            s_dma_slots[i].handle = NULL;
            s_dma_slots[i].owner = NULL;
            return;
        }
    }
}

/*
 * 检查缓冲是否在 DMA 可访问内存。
 *
 * F407 的 CCM（0x10000000..0x1000FFFF）由内核专用，DMA 访问会失败；
 * 这类错误在编译/链接阶段完全看不出来，必须在启动 DMA 前拒绝。
 * 这里不强制缓冲必须是 SRAM，因为从 Flash 读取常量做 TX 也是合法的。
 */
bool drv_dma_ptr_ok(const void *ptr, size_t length)
{
    uintptr_t start;
    uintptr_t end;

    if (ptr == NULL) return length == 0U;
    start = (uintptr_t)ptr;
    end = start + length;
    if (end < start) return false; /* 地址加法溢出 */
    /* 任意重叠都拒绝：start 在 CCM 内，或缓冲跨越 CCM 边界。 */
    if (start < 0x10010000U && end > 0x10000000U) return false;
    return true;
}

void drv_dma_irq_dispatch(uint8_t controller, uint8_t stream)
{
    drv_dma_route_t route = { controller, stream, 0U };
    size_t index = slot_index(&route);

    if (index >= DRV_DMA_SLOT_COUNT) return;
    if (s_dma_slots[index].handle == NULL) return;
    HAL_DMA_IRQHandler(s_dma_slots[index].handle);
}

/*
 * 16 个 DMA stream 中断入口。
 *
 * 这些名字必须与 CMSIS 启动文件中的弱符号完全一致；拼错不会编译失败，
 * 只会静默落到 Default_Handler。每个入口只做 RT-Thread 中断计数和
 * owner 分发，HT/TC/TE 的具体处理由 HAL 回调完成。
 */
#define DRV_DMA_IRQ_HANDLER(name, controller, stream)          \
    void name(void)                                             \
    {                                                           \
        rt_interrupt_enter();                                   \
        drv_dma_irq_dispatch((controller), (stream));           \
        rt_interrupt_leave();                                   \
    }

DRV_DMA_IRQ_HANDLER(DMA1_Stream0_IRQHandler, 1U, 0U)
DRV_DMA_IRQ_HANDLER(DMA1_Stream1_IRQHandler, 1U, 1U)
DRV_DMA_IRQ_HANDLER(DMA1_Stream2_IRQHandler, 1U, 2U)
DRV_DMA_IRQ_HANDLER(DMA1_Stream3_IRQHandler, 1U, 3U)
DRV_DMA_IRQ_HANDLER(DMA1_Stream4_IRQHandler, 1U, 4U)
DRV_DMA_IRQ_HANDLER(DMA1_Stream5_IRQHandler, 1U, 5U)
DRV_DMA_IRQ_HANDLER(DMA1_Stream6_IRQHandler, 1U, 6U)
DRV_DMA_IRQ_HANDLER(DMA1_Stream7_IRQHandler, 1U, 7U)
DRV_DMA_IRQ_HANDLER(DMA2_Stream0_IRQHandler, 2U, 0U)
DRV_DMA_IRQ_HANDLER(DMA2_Stream1_IRQHandler, 2U, 1U)
DRV_DMA_IRQ_HANDLER(DMA2_Stream2_IRQHandler, 2U, 2U)
DRV_DMA_IRQ_HANDLER(DMA2_Stream3_IRQHandler, 2U, 3U)
DRV_DMA_IRQ_HANDLER(DMA2_Stream4_IRQHandler, 2U, 4U)
DRV_DMA_IRQ_HANDLER(DMA2_Stream5_IRQHandler, 2U, 5U)
DRV_DMA_IRQ_HANDLER(DMA2_Stream6_IRQHandler, 2U, 6U)
DRV_DMA_IRQ_HANDLER(DMA2_Stream7_IRQHandler, 2U, 7U)

#undef DRV_DMA_IRQ_HANDLER
