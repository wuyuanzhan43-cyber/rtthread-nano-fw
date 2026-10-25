/*
 * fw/i2c.h - mr 风格 I2C 主模式设备类。
 *
 * 层级：src/framework/core
 * 职责：
 *   1. 每个 I2C 控制器对应一个设备：i2c1、i2c2、i2c3；
 *   2. 保存总线配置，提供原始收发、寄存器读写和设备探测；
 *   3. 只做参数检查、地址归一化和 ops 分发，不包含 HAL。
 *
 * 上下文：全部为阻塞调用，必须在任务上下文使用。
 * 依赖：fw/dev.h。
 *
 * 地址语义：
 *   - 类层使用 7 位从机地址（0x00..0x7F）；
 *   - fw_dev_read/fw_dev_write 的 off 也是 7 位地址，不是寄存器地址；
 *   - 寄存器地址只出现在 fw_i2c_mem_read/fw_i2c_mem_write 参数中。
 *
 * 事务语义：
 *   - transfer(tx != NULL && rx != NULL) 是“先写后读”的两个独立事务，
 *     中间会产生 STOP；
 *   - 需要 repeated start 的寄存器读必须使用 mem_read。
 */
#ifndef FW_I2C_H
#define FW_I2C_H

#include "fw/dev.h"

/* ioctl 命令：设置/读取 I2C 配置。参数均为 fw_i2c_config_t*。 */
#define FW_CTL_I2C_SET_CONFIG (0x08 << 24)
#define FW_CTL_I2C_GET_CONFIG (-(0x08 << 24))

/* HAL 的 I2C Size 参数是 uint16_t。 */
#define FW_I2C_MAX_TRANSFER 65535U

/*
 * I2C 配置。
 *
 * clock_hz - 总线频率；F407 支持 10 kHz..400 kHz（标准/快速模式）。
 *            400 kHz 需要外部 2.2k–4.7k 上拉，内部上拉通常不够。
 */
typedef struct {
    uint32_t clock_hz;
} fw_i2c_config_t;

/*
 * I2C 驱动操作集（第二层 ops）。
 *
 * configure - 把总线频率写入硬件并重新初始化；
 * transfer  - 原始事务：
 *               tx != NULL, rx == NULL -> 只写（一个 STOP）
 *               tx == NULL, rx != NULL -> 只读（一个 STOP）
 *               tx/rx 都非空          -> 先写后读（两个 STOP）
 *             返回 tx_len + rx_len；
 * mem_read  - 写寄存器地址后 repeated start 读；
 * mem_write - 写寄存器地址后写数据；
 * probe     - 探测地址是否有应答。
 *
 * mem_* / probe 可选；未实现时 helper 返回 FW_ENOTSUP。
 */
typedef struct {
    int (*configure)(fw_dev_t *dev, const fw_i2c_config_t *config);
    int (*transfer)(fw_dev_t *dev, uint16_t addr7,
                    const uint8_t *tx, size_t tx_len,
                    uint8_t *rx, size_t rx_len, uint32_t timeout_ms);
    int (*mem_read)(fw_dev_t *dev, uint16_t addr7, uint16_t mem_addr,
                    uint8_t mem_addr_size, uint8_t *rx, size_t rx_len,
                    uint32_t timeout_ms);
    int (*mem_write)(fw_dev_t *dev, uint16_t addr7, uint16_t mem_addr,
                     uint8_t mem_addr_size, const uint8_t *tx, size_t tx_len,
                     uint32_t timeout_ms);
    int (*probe)(fw_dev_t *dev, uint16_t addr7, uint32_t timeout_ms);
} fw_i2c_ops_t;

/*
 * I2C 设备对象。
 *
 * dev    - 设备基类，必须是第一个成员；
 * config - 当前总线配置的软件副本。
 */
typedef struct {
    fw_dev_t dev;
    fw_i2c_config_t config;
} fw_i2c_t;

/* 注册 I2C 设备；drv->type 必须为 FW_DEV_TYPE_I2C。 */
fw_status_t fw_i2c_register(fw_i2c_t *i2c, const char *name,
                            const fw_drv_t *drv,
                            const fw_i2c_config_t *config);

/* 按名字查找并做类型校验。 */
fw_i2c_t *fw_i2c_find(const char *name);

/*
 * 原始传输。
 * 返回 tx_len + rx_len；参数错误/驱动错误返回负值。
 * tx/rx 缓冲区在该次调用期间必须保持有效。
 */
int fw_i2c_transfer(fw_dev_t *dev, uint16_t addr7,
                    const uint8_t *tx, size_t tx_len,
                    uint8_t *rx, size_t rx_len, uint32_t timeout_ms);

/*
 * 寄存器读/写。
 * mem_addr_size 只能为 1 或 2；mem_read 使用 repeated start。
 */
int fw_i2c_mem_read(fw_dev_t *dev, uint16_t addr7, uint16_t mem_addr,
                    uint8_t mem_addr_size, uint8_t *rx, size_t rx_len,
                    uint32_t timeout_ms);

int fw_i2c_mem_write(fw_dev_t *dev, uint16_t addr7, uint16_t mem_addr,
                     uint8_t mem_addr_size, const uint8_t *tx, size_t tx_len,
                     uint32_t timeout_ms);

/*
 * 探测从机地址。
 * 返回 FW_OK（有应答）、FW_ENODEV（无应答/超时）、FW_EBUSY 或 FW_EIO。
 * 注意：总线被拉低卡死也可能表现为超时，需要结合硬件判断。
 */
fw_status_t fw_i2c_probe(fw_dev_t *dev, uint16_t addr7, uint32_t timeout_ms);

#endif
