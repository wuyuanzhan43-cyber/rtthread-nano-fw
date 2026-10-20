/*
 * drv_spi.h - STM32 SPI 实例表驱动接口。
 *
 * 层级：src/bsp/st/driver
 * 职责：按 Kconfig 的 FW_USING_SPIn 生成 spi1..N，并保存 HAL 句柄。
 * 上下文：configure/transfer 在任务上下文调用，允许阻塞。
 * 依赖：fw/spi.h、stm32f4xx_hal.h。
 *
 * 设计说明：
 *   SPI 是同步总线，没有 RX/TX ring 和中断事件；
 *   一次 transfer 在 HAL 中轮询完成，返回时数据已经发完/收完。
 *   片选由具体外设（如 ST7735S）用 pin 设备控制，驱动不管 CS。
 */
#ifndef DRV_SPI_H
#define DRV_SPI_H

#include "fw/spi.h"
#include "stm32f4xx_hal.h"

/*
 * 单个 SPI 实例。
 *
 * base     - SPI 类对象，必须放在第一个成员；
 * hal      - HAL 句柄，保存寄存器基址和初始化参数；
 * pclk     - 该 SPI 所在总线的时钟频率（SPI1 为 PCLK2，SPI2/3 为 PCLK1），
 *            用于计算分频；
 * instance - 实例编号 1..3，便于日志/调试；
 * drv      - 驱动描述，ops 指向本文件的 fw_spi_ops_t。
 */
typedef struct {
    fw_spi_t base;
    SPI_HandleTypeDef hal;
    uint32_t pclk;
    uint8_t instance;
    fw_drv_t drv;
} drv_spi_t;

/*
 * 注册所有已启用的 SPI。
 *
 * 对每个 FW_USING_SPIn=y 的实例：
 *   - 配置 SCK/MOSI（以及可选的 MISO）复用引脚；
 *   - 使能 SPI 时钟，计算 PCLK 和分频；
 *   - 调用 HAL_SPI_Init 并进入主模式；
 *   - 注册 spiN 设备。
 *
 * 返回 FW_OK 或第一个失败实例的错误码。
 */
fw_status_t drv_spi_register_all(void);

#endif
