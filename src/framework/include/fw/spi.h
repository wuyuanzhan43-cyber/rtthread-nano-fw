/*
 * fw/spi.h - mr 风格 SPI 设备类。
 *
 * 层级：src/framework/core
 * 职责：
 *   1. 每个 SPI 总线对应一个设备：spi1、spi2、spi3；
 *   2. 保存当前配置，并通过 ioctl 支持运行期修改；
 *   3. 提供同步 transfer 接口，由 bsp 驱动映射到 HAL。
 *
 * 上下文：configure/transfer 都在任务上下文调用，可能阻塞。
 * 依赖：fw/dev.h。
 *
 * 与 serial 的差异：
 *   - SPI 是总线而不是字节流，没有 ring，也没有中断事件；
 *   - 传输长度通常较短（LCD 窗口/像素），直接同步等待 HAL 完成；
 *   - 片选（CS）由具体设备（如 ST7735S）通过 pin 设备控制，
 *     SPI 类只负责时钟、相位、位序和速率。
 */
#ifndef FW_SPI_H
#define FW_SPI_H

#include "fw/dev.h"

/* ioctl 命令：设置/读取 SPI 配置。参数均为 fw_spi_config_t*。 */
#define FW_CTL_SPI_SET_CONFIG (0x08 << 24)
#define FW_CTL_SPI_GET_CONFIG (-(0x08 << 24))

/*
 * SPI 配置。
 *
 * max_hz    - 期望的最大时钟频率，驱动会向下取整到可用的分频；
 * mode      - CPOL/CPHA 组合，0..3，对应模式 0/1/2/3；
 * bit_order - 0 表示 LSB first，1 表示 MSB first。
 *
 * 注意：max_hz 不是精确频率，而是上限；实际频率由 PCLK 分频决定。
 */
typedef struct {
    uint32_t max_hz;
    int mode;
    int bit_order;  /* 0=LSB, 1=MSB */
} fw_spi_config_t;

/*
 * SPI 驱动操作集（第二层 ops）。
 *
 * configure - 把软件配置写入硬件并重新初始化外设；
 * transfer  - 同步传输：
 *               tx != NULL, rx != NULL -> 全双工
 *               tx != NULL, rx == NULL -> 只发
 *               tx == NULL, rx != NULL -> 只收
 *             返回实际传输字节数或负错误码。
 */
typedef struct {
    int (*configure)(fw_dev_t *dev, const fw_spi_config_t *config);
    int (*transfer)(fw_dev_t *dev, const uint8_t *tx, uint8_t *rx,
                    size_t length, uint32_t timeout_ms);
} fw_spi_ops_t;

/*
 * SPI 设备对象。
 *
 * dev    - 设备基类，必须是第一个成员；
 * config - 当前配置的软件副本，GET_CONFIG 直接返回。
 *
 * 设备没有 open/close/read/write/isr：SPI 的所有动作都通过
 * ioctl(配置) 和 fw_spi_transfer(传输) 完成。
 */
typedef struct {
    fw_dev_t dev;
    fw_spi_config_t config;
} fw_spi_t;

/*
 * 注册 SPI 设备。
 * drv->type 必须为 FW_DEV_TYPE_SPI；config 不能为空。
 * 驱动通常在板级初始化时已经完成 GPIO/时钟配置。
 */
fw_status_t fw_spi_register(fw_spi_t *spi, const char *name,
                            const fw_drv_t *drv,
                            const fw_spi_config_t *config);

/* 按名字查找，并做类型校验。 */
fw_spi_t *fw_spi_find(const char *name);

/*
 * 同步传输。
 * 参数检查由类完成：
 *   - length == 0 返回 0；
 *   - tx 和 rx 同时为空返回 FW_EINVAL；
 *   - 驱动未实现 transfer 返回 FW_ENOTSUP。
 * 返回值大于等于 0 表示传输字节数。
 */
int fw_spi_transfer(fw_dev_t *dev, const uint8_t *tx, uint8_t *rx,
                    size_t length, uint32_t timeout_ms);

#endif
