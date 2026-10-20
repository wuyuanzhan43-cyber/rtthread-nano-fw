/*
 * fw/serial.h - mr 风格串口设备类。
 *
 * 层级：src/framework/core
 * 职责：
 *   1. 每个启用的 UART 对应一个设备：serial1、serial2、serial3；
 *   2. 管理 RX/TX 两个静态 ring，向应用提供统一 read/write/ioctl；
 *   3. 通过驱动 ops 把硬件细节（寄存器、中断、事件）留在 bsp 层。
 *
 * 上下文：
 *   - open/read/write/ioctl/wait_rx：任务上下文；
 *   - isr：只由驱动在 USART 中断中调用，必须非阻塞。
 *
 * 依赖：fw/dev.h、fw/ring.h。
 *
 * 数据流概览：
 *
 *   RX: USART RXNE -> 驱动 IRQ -> fw_dev_isr(RD) -> 类写入 rd_fifo
 *       -> rt_event_send -> 等待中的任务醒来 -> serial_read 拷贝给应用
 *
 *   TX: 应用 write -> 类写入 wr_fifo -> 驱动打开 TXE 中断
 *       -> 每次 TXE 中断取一字节写 DR -> ring 空时关闭 TXE
 *
 * 并发模型：
 *   任务和 ISR 共享两个 ring；类本身不包含 RTOS 头文件，
 *   由驱动通过 lock/unlock 注入“关中断/开中断”临界区。
 */
#ifndef FW_SERIAL_H
#define FW_SERIAL_H

#include "fw/dev.h"
#include "fw/ring.h"

/* 串口数据位：当前驱动只支持 8 位，宏保留给配置结构体使用。 */
#define FW_SERIAL_DATA_BITS_8 8

/* 停止位：1 位或 2 位。 */
#define FW_SERIAL_STOP_BITS_1 1
#define FW_SERIAL_STOP_BITS_2 2

/* 校验位：无、偶、奇。 */
#define FW_SERIAL_PARITY_NONE 0
#define FW_SERIAL_PARITY_EVEN 1
#define FW_SERIAL_PARITY_ODD 2

/*
 * ioctl 命令编号。
 *
 * 高字节表示命令类别：设置类用正数，查询类用对应的负数，
 * 这样调用者可以通过符号快速区分“写配置”和“读状态”。
 * 低 24 位预留给偏移或命令参数。
 */
#define FW_CTL_SERIAL_SET_CONFIG (0x08 << 24)   /* 参数：fw_serial_config_t* */
#define FW_CTL_SERIAL_GET_CONFIG (-(0x08 << 24))/* 参数：fw_serial_config_t* */
#define FW_CTL_SERIAL_CLR_RD_BUF (0x0b << 24)   /* 清空接收 ring */
#define FW_CTL_SERIAL_CLR_WR_BUF (0x0c << 24)   /* 清空发送 ring */
#define FW_CTL_SERIAL_GET_RD_DATASZ (-(0x0b << 24)) /* 参数：size_t* */
#define FW_CTL_SERIAL_GET_WR_DATASZ (-(0x0c << 24)) /* 参数：size_t* */

/*
 * 中断事件号。
 *
 * 驱动在 IRQ 中调用 fw_dev_isr(dev, event, NULL)：
 *   RD  - 收到一个字节（RXNE），类负责读 DR 并写入 rd_fifo；
 *   WR  - 发送寄存器空（TXE），类负责从 wr_fifo 取下一个字节；
 *   ERR - 溢出/帧错误/噪声/校验错误，类负责记录错误状态。
 *
 * 事件号是位标志，方便以后一次上报多个事件。
 */
#define FW_ISR_SERIAL_RD 0x01
#define FW_ISR_SERIAL_WR 0x02
#define FW_ISR_SERIAL_ERR 0x04

/*
 * 串口配置。
 *
 * baud       - 波特率，必须大于 0；
 * data_bits  - 当前仅支持 8；
 * stop_bits  - 1 或 2；
 * parity     - FW_SERIAL_PARITY_*。
 *
 * 配置在 open 时下发给驱动；运行期可通过 SET_CONFIG 修改。
 */
typedef struct {
    uint32_t baud;
    int data_bits;
    int stop_bits;
    int parity;
} fw_serial_config_t;

/*
 * 串口驱动操作集（第二层 ops）。
 *
 * 这些函数由 bsp 驱动实现，类只按语义调用，不认识 USART 寄存器。
 *
 * configure  - 应用配置到硬件（HAL_UART_Init）；
 * read_byte  - 从数据寄存器取一个字节，返回 0..255 或负错误码；
 * write_byte - 把一字节写入数据寄存器；
 * start_tx   - 确保 TXE 中断已打开，开始/继续发送；
 * stop_tx    - 关闭 TXE 中断，表示发送完成；
 * wait_rx    - 等待 RX 事件，RTOS 相关实现放在驱动中；
 * lock       - 进入临界区，返回平台相关的“中断级别”；
 * unlock     - 退出临界区，参数是 lock 的返回值。
 *
 * 为什么 wait_rx/lock 放在驱动而不是 core：
 *   这样 framework/core 不需要 include rtthread.h，
 *   同一套 serial 类可以复用到裸机或其他 RTOS 的移植层。
 */
typedef struct {
    int (*configure)(fw_dev_t *dev, const fw_serial_config_t *config);
    int (*read_byte)(fw_dev_t *dev);
    int (*write_byte)(fw_dev_t *dev, uint8_t byte);
    int (*start_tx)(fw_dev_t *dev);
    int (*stop_tx)(fw_dev_t *dev);
    fw_status_t (*wait_rx)(fw_dev_t *dev, uint32_t timeout_ms);
    int (*lock)(fw_dev_t *dev);
    void (*unlock)(fw_dev_t *dev, int level);
} fw_serial_ops_t;

/*
 * 串口设备对象（类层）。
 *
 * dev      - 设备基类，必须放在第一个成员；
 * config   - 当前配置的软件副本，GET_CONFIG 直接返回它；
 * rd_fifo  - 接收 ring，由驱动 ISR 写、任务读；
 * wr_fifo  - 发送 ring，由任务写、驱动 ISR 读；
 * error    - 最近一次错误，ISR 可写，任务读取后清零。
 *
 * ring 的存储区不在这个结构体里：由驱动提供静态数组，
 * 通过 fw_serial_register 传进来，便于按 Kconfig 调整大小。
 */
typedef struct {
    fw_dev_t dev;
    fw_serial_config_t config;
    fw_ring_t rd_fifo;
    fw_ring_t wr_fifo;
    volatile fw_status_t error;
} fw_serial_t;

/*
 * 注册串口设备。
 *
 * 参数：
 *   serial     - 静态分配的 fw_serial_t（通常嵌在 drv_serial_t 中）；
 *   name       - 设备名，如 "serial1"；
 *   drv        - 类型必须为 FW_DEV_TYPE_SERIAL，ops 为 fw_serial_ops_t；
 *   rd_storage - 接收 ring 的静态存储区；
 *   rd_size    - 接收 ring 容量（字节）；
 *   wr_storage - 发送 ring 的静态存储区；
 *   wr_size    - 发送 ring 容量（字节）；
 *   config     - 初始配置；NULL 时使用 115200-8-N-1。
 *
 * 返回 FW_OK 或错误码。ring 容量为 0、驱动类型不匹配都会返回 FW_EINVAL。
 */
fw_status_t fw_serial_register(fw_serial_t *serial, const char *name,
                               const fw_drv_t *drv,
                               uint8_t *rd_storage, size_t rd_size,
                               uint8_t *wr_storage, size_t wr_size,
                               const fw_serial_config_t *config);

/* 按名字查找串口对象；名字存在但类型不是 serial 时返回 NULL。 */
fw_serial_t *fw_serial_find(const char *name);

/* 修改配置的便捷函数，等价于 ioctl(SET_CONFIG)。 */
int fw_serial_ioctl_config(fw_dev_t *dev, const fw_serial_config_t *config);

/*
 * 等待接收数据。
 *
 * 与 read 的区别：
 *   - read 会尝试从 ring 拷贝数据；
 *   - wait_rx 只等待“有数据或出错”，不消费数据，适合主循环先等事件
 *     再批量读取的场景。
 *
 * 返回 FW_OK 表示 ring 中已有数据或存在待处理错误；
 * FW_ETIMEOUT 表示超时；FW_EAGAIN 表示非阻塞且暂无数据。
 */
fw_status_t fw_serial_wait_rx(fw_dev_t *dev, uint32_t timeout_ms);

#endif
