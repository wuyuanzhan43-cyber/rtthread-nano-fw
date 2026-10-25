/*
 * fw/spi.h - mr 风格 SPI 设备类。
 *
 * 层级：src/framework/core
 * 职责：
 *   1. 每个 SPI 总线对应一个设备：spi1、spi2、spi3；
 *   2. 保存当前配置，并通过 ioctl 支持运行期修改；
 *   3. 提供同步 transfer 与异步 async/wait/abort 接口，
 *      由 bsp 驱动映射到 HAL 轮询或 DMA。
 *
 * 上下文：configure/transfer 都在任务上下文调用，可能阻塞；
 *         async 只启动硬件，wait 才阻塞。
 * 依赖：fw/dev.h。
 *
 * 与 serial 的差异：
 *   - SPI 是总线而不是字节流，没有 ring，也没有中断事件；
 *   - 传输长度通常较短（LCD 窗口/像素），同步路径直接等待完成；
 *   - 片选（CS）由具体设备（如 ST7735S）通过 pin 设备控制，
 *     SPI 类只负责时钟、相位、位序和速率。
 *
 * 异步传输契约：
 *   - async 返回后 TX/RX 缓冲仍被硬件使用，直到 wait/abort 返回；
 *   - async 期间调用者必须保持 CS 有效，不能发起第二个 owner；
 *   - SPI 对象没有锁，同一总线同一时刻只允许一个调用者使用。
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

/* HAL 的 SPI Size 参数是 uint16_t；单次传输不能超过该值。 */
#define FW_SPI_MAX_TRANSFER 65535U

/*
 * SPI 驱动操作集（第二层 ops）。
 *
 * configure - 把软件配置写入硬件并重新初始化外设；
 * transfer  - 同步传输：
 *               tx != NULL, rx != NULL -> 全双工
 *               tx != NULL, rx == NULL -> 只发
 *               tx == NULL, rx != NULL -> 只收
 *             返回实际传输字节数或负错误码。
 *
 * transfer_async/wait/abort 是可选扩展，三者必须同时实现：
 *   transfer_async - 启动传输后立即返回 length（硬件在后台搬运）；
 *   transfer_wait  - 等待完成；timeout==0 时只轮询，未完成返回 FW_EAGAIN；
 *                    超时返回 FW_ETIMEOUT，由类层决定是否 abort；
 *   transfer_abort - 取消在途传输并确保硬件回到可再次启动的状态。
 * 驱动不实现时保持为 NULL，类层返回 FW_ENOTSUP。
 */
typedef struct {
    int (*configure)(fw_dev_t *dev, const fw_spi_config_t *config);
    int (*transfer)(fw_dev_t *dev, const uint8_t *tx, uint8_t *rx,
                    size_t length, uint32_t timeout_ms);
    int (*transfer_async)(fw_dev_t *dev, const uint8_t *tx, uint8_t *rx,
                          size_t length);
    int (*transfer_wait)(fw_dev_t *dev, uint32_t timeout_ms);
    int (*transfer_abort)(fw_dev_t *dev);
} fw_spi_ops_t;

/*
 * SPI 设备对象。
 *
 * dev    - 设备基类，必须是第一个成员；
 * config - 当前配置的软件副本，GET_CONFIG 直接返回；
 * busy   - 是否有异步传输在途（由 async/wait/abort 维护）。
 *
 * 设备没有 open/close/read/write/isr：SPI 的所有动作都通过
 * ioctl(配置) 与 fw_spi_transfer / fw_spi_transfer_async(传输) 完成。
 */
typedef struct {
    fw_dev_t dev;
    fw_spi_config_t config;
    volatile bool busy;
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
 *   - busy（有异步传输在途）返回 FW_EBUSY；
 *   - length == 0 返回 0；
 *   - tx 和 rx 同时为空返回 FW_EINVAL；
 *   - length > FW_SPI_MAX_TRANSFER 返回 FW_EINVAL；
 *   - 驱动未实现 transfer 返回 FW_ENOTSUP。
 * 返回值大于等于 0 表示传输字节数。
 */
int fw_spi_transfer(fw_dev_t *dev, const uint8_t *tx, uint8_t *rx,
                    size_t length, uint32_t timeout_ms);

/*
 * 启动异步传输。
 *
 * 成功返回 length；失败返回负错误码。返回后硬件可能仍在搬运，
 * 调用者必须再调用 fw_spi_transfer_wait 或 fw_spi_transfer_abort。
 * tx/rx 缓冲和 CS 在 wait/abort 返回前必须保持有效。
 *
 * 错误码：FW_EINVAL（参数/长度非法）、FW_EBUSY（已有传输在途）、
 *         FW_ENOTSUP（驱动不支持异步）、驱动自定义负值。
 */
int fw_spi_transfer_async(fw_dev_t *dev, const uint8_t *tx, uint8_t *rx,
                          size_t length);

/*
 * 等待异步传输结束。
 *
 * timeout_ms 语义：
 *   0            - 只轮询；未完成返回 FW_EAGAIN，传输继续运行；
 *   FW_WAIT_FOREVER - 永久等待；
 *   其他         - 等待指定毫秒。
 *
 * 返回值：
 *   FW_OK      - 传输成功，busy 清除；
 *   FW_EIO     - 硬件错误，busy 清除（若清理失败则保持 busy）；
 *   FW_ETIMEOUT- 等待超时；类层已尝试 abort，abort 成功才清 busy；
 *   FW_EAGAIN  - 仅 timeout==0 且未完成，busy 保持；
 *   FW_EINVAL  - 当前没有在途传输，或驱动不支持异步。
 */
fw_status_t fw_spi_transfer_wait(fw_dev_t *dev, uint32_t timeout_ms);

/*
 * 取消在途异步传输。
 *
 * 未忙时幂等返回 FW_OK。abort 成功后 busy 清除；
 * abort 失败返回驱动错误码并保持 busy，调用者可以再次尝试。
 */
fw_status_t fw_spi_transfer_abort(fw_dev_t *dev);

#endif
