/*
 * drv_spi.h - STM32 SPI 实例表驱动接口。
 *
 * 层级：src/platform/stm32f407
 * 职责：接收板级 SPI 描述符，按 FW_USING_SPIn 注册 spi1..N。
 * 上下文：configure/transfer 在任务上下文调用，允许阻塞；
 *         DMA 完成/错误回调在中断上下文。
 * 依赖：fw/spi.h、drv_dma.h、rtthread.h、stm32f4xx_hal.h。
 *
 * 设计说明：
 *   - 默认（FW_USING_SPIn_DMA=n）保持 HAL 轮询，行为与旧版一致；
 *   - 打开 DMA 后，长传输走 HAL DMA + RT-Thread 信号量，
 *     短传输仍轮询，避免 1 字节命令承担调度开销；
 *   - TX 始终可用；RX 需要 use_miso=true 且板级分配了 RX route；
 *   - 片选由具体外设（如 ST7735S）用 pin 设备控制，驱动不管 CS。
 */
#ifndef DRV_SPI_H
#define DRV_SPI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "fw/spi.h"
#include "drv_dma.h"
#include <rtthread.h>
#include "stm32f4xx_hal.h"

/*
 * 是否存在任一启用的 SPI DMA 实例。
 *
 * 编译期裁剪：默认 profile 完全关闭 SPI DMA 时，不编译 DMA 状态字段、
 * HAL DMA 完成回调和 SPIx_IRQHandler，避免把 HAL SPI DMA 代码链接进来。
 * FW_USING_SPIn_DMA 来自生成的 fw_config.h（经 fw/dev.h 引入）。
 */
#if (defined(FW_USING_SPI1) && FW_USING_SPI1 && \
     defined(FW_USING_SPI1_DMA) && FW_USING_SPI1_DMA) || \
    (defined(FW_USING_SPI2) && FW_USING_SPI2 && \
     defined(FW_USING_SPI2_DMA) && FW_USING_SPI2_DMA) || \
    (defined(FW_USING_SPI3) && FW_USING_SPI3 && \
     defined(FW_USING_SPI3_DMA) && FW_USING_SPI3_DMA)
#define DRV_SPI_USE_DMA 1
#else
#define DRV_SPI_USE_DMA 0
#endif

/*
 * 板级 SPI 描述符。
 *
 * instance - SPI 编号 1..3；
 * sck/mosi/miso - 端口 0=A..4=E、引脚号、复用功能号；
 * use_miso - false 时配置为 1LINE，只允许发送；
 * max_hz   - 初始频率上限；
 * dma_rx/dma_tx - board_config.h 中手写的 DMA 路由；
 * dma_tx_enabled/dma_rx_enabled - 是否使用该方向的 DMA。
 *                                  RX 要求 use_miso=true。
 */
typedef struct {
    uint8_t instance;
    uint8_t sck_port;
    uint8_t sck_pin;
    uint8_t sck_af;
    uint8_t mosi_port;
    uint8_t mosi_pin;
    uint8_t mosi_af;
    uint8_t miso_port;
    uint8_t miso_pin;
    uint8_t miso_af;
    bool use_miso;
    uint32_t max_hz;
    drv_dma_route_t dma_rx;
    drv_dma_route_t dma_tx;
    bool dma_tx_enabled;
    bool dma_rx_enabled;
} drv_spi_hw_t;

/*
 * 单个 SPI 实例。
 *
 * base     - SPI 类对象，必须放在第一个成员；
 * hal      - HAL 句柄；
 * pclk     - 所在总线时钟；
 * instance - 实例编号 1..3；
 * use_miso - 是否双线；false 时拒绝 rx 传输；
 * hdma_*   - 由本驱动持有并初始化的 DMA 句柄；
 * dma_*_route - claim 时使用的 controller/stream/channel；
 * dma_done - 传输完成/错误时由 ISR release 的信号量；
 * active   - 硬件是否仍在搬运（用于抑制迟到回调）；
 * dma_result - 最近一次完成的 FW_OK/FW_EIO；
 * drv      - 驱动描述。
 */
typedef struct {
    fw_spi_t base;
    SPI_HandleTypeDef hal;
    uint32_t pclk;
    uint8_t instance;
    bool use_miso;

#if DRV_SPI_USE_DMA
    DMA_HandleTypeDef hdma_tx;
    DMA_HandleTypeDef hdma_rx;
    drv_dma_route_t dma_tx_route;
    drv_dma_route_t dma_rx_route;
    struct rt_semaphore dma_done;
    volatile bool active;
    volatile int dma_result;
    bool dma_tx_enabled;
    bool dma_rx_enabled;
#endif

    fw_drv_t drv;
} drv_spi_t;

/* 按板级描述符注册 SPI；返回第一个失败实例的错误码。 */
fw_status_t drv_spi_register_all(const drv_spi_hw_t *hw, size_t count);

#endif
