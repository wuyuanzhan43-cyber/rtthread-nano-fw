/*
 * fw/dev.h - mr-library 风格设备核心（接口定义）。
 *
 * 层级：src/framework/core
 * 职责：
 *   1. 定义所有设备共用的 fw_dev_t、fw_drv_t、fw_dev_ops_t；
 *   2. 声明“路径 -> 设备”的注册、查找与统一读写入口；
 *   3. 为上层提供 open/close/read/write/ioctl/isr 六个标准动作。
 *
 * 上下文：
 *   - 注册：rt_hw_board_init() 内、调度器启动之前完成，之后设备表只读；
 *   - open/read/write/ioctl：任务上下文；
 *   - isr：只允许由驱动在中断服务函数中调用，不能阻塞。
 *
 * 依赖：fw/status.h、fw_config.h、stddef.h、stdint.h。
 *
 * 设计说明（与 mr-library 的关系）：
 *   - 保留 mr-library 的路径寻址、ops/drv 分离、offset 语义和引用计数；
 *   - 不引入动态设备树、descriptor 表、读写锁和 mr_malloc：
 *     本项目所有设备、缓冲区和同步对象都是静态分配，RAM 可预测；
 *   - 设备表是固定容量数组，查找是线性扫描。设备数量在 16 个左右，
 *     线性查找比哈希表更简单，也不需要动态内存。
 *
 * 命名约定：
 *   - fw_dev_t 是“基类”，必须作为具体设备结构体的第一个成员；
 *   - 具体类（fw_serial_t 等）通过 FW_CONTAINER_OF 从基类指针反推外层对象。
 */
#ifndef FW_DEV_H
#define FW_DEV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "fw/status.h"
#include "fw_config.h"

/*
 * 设备表容量。
 * 该值可由 Kconfig 的 FW_DEV_MAX 覆盖；这里给出编译期兜底默认值，
 * 便于 host 测试或不经过生成的独立编译。
 */
#ifndef FW_DEV_MAX
#define FW_DEV_MAX 16
#endif

/*
 * 设备名最大长度（含结尾 '\0'）。
 * 路径会被归一化为 "serial1" 这种短名，正常不会超过 16 字节。
 */
#define FW_DEV_NAME_MAX 16

/*
 * 由“基类成员地址”反推“外层对象地址”。
 *
 * 使用前提：member 必须是 type 的第一个成员，或者至少 offsetof 合法。
 * 本项目中 fw_dev_t 永远放在具体设备结构体的第一个位置，因此
 * FW_CONTAINER_OF(dev, fw_serial_t, dev) 等价于把基类指针转回 serial 对象。
 *
 * 典型用途：
 *   fw_dev_t *dev = fw_dev_find("serial1");
 *   fw_serial_t *serial = FW_CONTAINER_OF(dev, fw_serial_t, dev);
 */
#define FW_CONTAINER_OF(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

/*
 * open() 的访问模式标志。
 * 当前设备实现只做引用计数，不根据 oflags 限制读写方向；
 * 保留这些定义是为了与 mr-library/POSIX 的调用习惯保持一致，
 * 后续如果引入只读设备，可以在 class 层检查 oflags。
 */
#define FW_OFLAG_CLOSED 0
#define FW_OFLAG_RDONLY 0x01
#define FW_OFLAG_WRONLY 0x02
#define FW_OFLAG_RDWR 0x03
#define FW_OFLAG_NONBLOCK 0x04

/*
 * 设备类型令牌。
 *
 * 这些值用于两件事：
 *   1. fw_dev_find() 之后做类型检查（fw_serial_find 等）；
 *   2. 驱动注册时校验 drv->type 与类是否匹配，避免把 serial 驱动
 *      误注册到 pin 设备上。
 *
 * 新增设备类时在这里追加一个唯一值，并保持已有值不变，
 * 避免影响已经写进生成配置或测试代码的类型判断。
 */
enum fw_dev_type {
    FW_DEV_TYPE_NONE = 0,
    FW_DEV_TYPE_PIN = 1,
    FW_DEV_TYPE_SERIAL,
    FW_DEV_TYPE_SPI,
    FW_DEV_TYPE_DISPLAY,
    FW_DEV_TYPE_I2C,
    FW_DEV_TYPE_ADC,
    FW_DEV_TYPE_PWM
};

struct fw_dev;

/*
 * 设备类操作集（第一层 ops）。
 *
 * 这些函数由“设备类”实现（framework/core 下的 .c 文件），由 fw_dev_* 统一分发。
 * 具体硬件差异不在这里处理，而是由类再调用底层驱动的 ops（第二层 ops）。
 *
 * 参数约定：
 *   off        - 设备内偏移。pin 类中是编码后的引脚号；serial/spi 目前忽略；
 *   buf/size   - 调用者提供的缓冲区与长度；
 *   timeout_ms - 0 表示非阻塞；FW_WAIT_FOREVER 表示永久等待；
 *   event/args - ISR 事件号与可选参数。
 *
 * 返回值：
 *   - read/write 成功时返回实际字节数（>= 0）；
 *   - 失败时返回 fw_status_t 的负值（FW_EINVAL、FW_EIO 等）；
 *   - ioctl 成功通常返回 sizeof(参数结构体)，失败返回负值。
 */
typedef struct {
    /*
     * 首次打开时调用（ref_count 从 0 变 1）。
     * 类通常在这里清空缓冲、恢复默认配置并通知驱动重新配置硬件。
     * 允许为 NULL，表示该设备没有额外的打开动作。
     */
    fw_status_t (*open)(struct fw_dev *dev);

    /*
     * 最后一次关闭时调用（ref_count 从 1 变 0）。
     * 允许为 NULL；当前 pin/spi 类没有关闭动作。
     */
    fw_status_t (*close)(struct fw_dev *dev);

    /*
     * 读取数据。
     * 对 serial：从 RX ring 取数据，可能阻塞等待；
     * 对 pin：size 个连续引脚号，每个字节 0/1；
     * 对 spi/display：保留未实现。
     */
    int (*read)(struct fw_dev *dev, int off, void *buf, size_t size,
                uint32_t timeout_ms);

    /*
     * 写入数据。
     * 对 serial：写入 TX ring 并启动发送，返回“已接收”的字节数，
     *            可能小于 size（ring 满时短写），调用者需要重试；
     * 对 pin：连续写 size 个引脚；
     * 对 spi/display：保留未实现。
     */
    int (*write)(struct fw_dev *dev, int off, const void *buf, size_t size,
                 uint32_t timeout_ms);

    /*
     * 控制接口。
     * cmd 的编码习惯：高字节放命令号，低 24 位保留给偏移/参数。
     * 具体命令定义在各设备类的头文件中（FW_CTL_SERIAL_* 等）。
     */
    int (*ioctl)(struct fw_dev *dev, int off, int cmd, void *args);

    /*
     * 中断事件入口。
     * 只允许驱动在 ISR 中调用；实现必须短小、不可阻塞、不可调用
     * rt_thread_mdelay 等会切换线程的 API。
     */
    int (*isr)(struct fw_dev *dev, int event, void *args);
} fw_dev_ops_t;

/*
 * 驱动描述（第二层 ops + 私有数据）。
 *
 * type 必须与注册的设备类型一致；
 * ops 指向“具体类 ops”（例如 fw_serial_ops_t），由类在调用前做强制转换；
 * data 是驱动私有数据（例如 drv_serial_t 自身），类不解释其内容。
 *
 * 这样分层的好处：
 *   - framework/core 只认识 fw_dev_ops_t，不认识 HAL；
 *   - bsp 驱动只实现类规定的函数表，不关心注册表；
 *   - 同一个类可以挂不同芯片的驱动，只要 ops 语义一致。
 */
typedef struct {
    int type;          /* FW_DEV_TYPE_*，注册时做匹配校验 */
    const void *ops;   /* 指向具体类的 ops 表，例如 fw_serial_ops_t */
    void *data;        /* 驱动私有上下文，当前多为驱动实例自身 */
} fw_drv_t;

/*
 * 设备基类。
 *
 * 内存布局要求：本结构必须是具体设备结构体的第一个成员。
 * 这样 fw_dev_t* 与具体设备对象指针可以互相转换，且注册表只保存
 * 基类指针，不需要为每种设备维护不同类型的数组。
 *
 * 字段说明：
 *   name       - 归一化后的短名，例如 "serial1"（不含 /dev/ 前缀）；
 *   type       - FW_DEV_TYPE_*，由类在注册时填写；
 *   ref_count  - open 引用计数。0 表示关闭；>0 表示至少有一个使用者；
 *   ops        - 设备类操作集，由类在注册后设置；
 *   drv        - 驱动描述，由 bsp 驱动在注册前准备好。
 */
typedef struct fw_dev {
    char name[FW_DEV_NAME_MAX];
    int type;
    size_t ref_count;
    const fw_dev_ops_t *ops;
    const fw_drv_t *drv;
    volatile uint32_t error_count; /* 类层累计的负结果次数，用于诊断 */
} fw_dev_t;

/*
 * 注册一个设备。
 *
 * 参数：
 *   dev  - 由驱动/板级静态分配的设备对象，不能是栈变量；
 *   name - "serial1" 或 "/dev/serial1"，函数内部会归一化；
 *   type - FW_DEV_TYPE_*；
 *   drv  - 驱动描述，注册后必须保持有效（通常为静态对象）。
 *
 * 返回：
 *   FW_OK      注册成功；
 *   FW_EINVAL  参数非法、名字为空或过长；
 *   FW_EEXIST  同名设备已经注册；
 *   FW_EOVERFLOW 设备表已满（FW_DEV_MAX）。
 *
 * 上下文：调度器启动前调用。若在运行期注册，需要由调用者保证互斥。
 */
fw_status_t fw_dev_register(fw_dev_t *dev, const char *name, int type,
                            const fw_drv_t *drv);

/*
 * 按名字查找设备。
 * 支持 "serial1" 与 "/dev/serial1" 两种写法；找不到返回 NULL。
 * 查找过程只读设备表，可在任务上下文调用；不在 ISR 中推荐使用。
 */
fw_dev_t *fw_dev_find(const char *name);

/* 返回当前已注册设备数量，主要用于启动日志与 Host 测试。 */
size_t fw_dev_count(void);

/*
 * 遍历设备表。
 * visit 不能为 NULL；遍历期间不要注册新设备，否则行为未定义。
 * 典型用途：启动时打印设备清单、测试时统计类型。
 */
void fw_dev_walk(void (*visit)(fw_dev_t *dev, void *arg), void *arg);

/* 清空注册表，仅供 Host 单元测试在用例之间复位使用；固件中不要调用。 */
void fw_dev_registry_reset(void);

/*
 * 打开设备。
 *
 * 语义：
 *   - ref_count 从 0 变 1 时调用一次 ops->open；
 *   - 之后重复 open 只增加引用计数；
 *   - 打开失败时引用计数保持不变。
 *
 * oflags 目前只作为接口保留，类实现可以选择忽略。
 * 返回 FW_OK 或类 open 返回的错误码。
 */
fw_status_t fw_dev_open(fw_dev_t *dev, int oflags);

/*
 * 关闭设备。
 *
 * 语义：
 *   - 每次 close 将 ref_count 减 1；
 *   - 减到 0 时调用一次 ops->close；
 *   - 多余 close 或从未 open 时返回 FW_EINVAL。
 */
fw_status_t fw_dev_close(fw_dev_t *dev);

/*
 * 统一读入口。
 * 参数检查在 core 完成（空指针、size==0），随后原样转发给 ops->read。
 * size==0 直接返回 0，不触发驱动调用。
 */
int fw_dev_read(fw_dev_t *dev, int off, void *buf, size_t size,
                uint32_t timeout_ms);

/*
 * 统一写入口。
 * 返回值可能是“短写”计数；调用者应检查返回值并按需重试剩余部分。
 */
int fw_dev_write(fw_dev_t *dev, int off, const void *buf, size_t size,
                 uint32_t timeout_ms);

/*
 * 统一控制入口。
 * 设备没有实现 ioctl 时返回 FW_ENOTSUP，而不是让空指针崩溃。
 */
int fw_dev_ioctl(fw_dev_t *dev, int off, int cmd, void *args);

/*
 * 统一中断入口。
 * 只应由 bsp 驱动的 IRQ handler 调用；设备没有 isr 时返回 FW_ENOTSUP。
 */
int fw_dev_isr(fw_dev_t *dev, int event, void *args);

/* 类型判断辅助函数：dev 非空且 type 匹配时返回 true。 */
bool fw_dev_is_type(const fw_dev_t *dev, int type);

/*
 * 设备错误计数。
 *
 * 类层在驱动返回负值时调用 note_error；count 只增不减，供诊断/RPC
 * 或调试器读取。计数不是原子操作：同一设备应保持单 owner 使用。
 */
void fw_dev_note_error(fw_dev_t *dev);
uint32_t fw_dev_error_count(const fw_dev_t *dev);

#endif
