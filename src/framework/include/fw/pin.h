/*
 * fw/pin.h - mr 风格 GPIO 设备类。
 *
 * 层级：src/framework/core
 * 职责：
 *   1. 用一个名为 "pin" 的设备覆盖芯片所有 GPIO；
 *   2. 用 offset（引脚号）区分具体引脚，而不是为每个引脚创建对象；
 *   3. 提供读电平、写电平、切换输入/输出/模拟模式；
 *   4. 提供 EXTI attach/detach/enable 和 ISR 回调分派。
 *
 * 上下文：
 *   - read/write 只访问寄存器，可在任务或 ISR 中调用；
 *   - configure/attach/detach 会配置硬件，只在任务上下文调用；
 *   - isr 由驱动在 EXTI 中断中调用，回调必须非阻塞。
 *
 * 依赖：fw/dev.h。
 *
 * 为什么只用一个设备：
 *   - STM32F407VE 有 5 个端口、80 个引脚，如果每个引脚一个设备，
 *     设备表很快被占满，且注册代码冗长；
 *   - mr-library 的 pin 编号方式正好适合“一个设备 + 编号”的模型；
 *   - 应用只需要记住编号，不需要持有几十个设备指针。
 */
#ifndef FW_PIN_H
#define FW_PIN_H

#include <stdbool.h>
#include "fw/dev.h"

/*
 * 静态 IRQ 回调表容量；可由 Kconfig 的 FW_PIN_IRQ_MAX 覆盖。
 * 一条表项对应一个已 attach 的引脚，而不是一条 EXTI 线；
 * 硬件仍只有 16 条 EXTI 线，容量超过 16 时兼容层会拒绝 attach。
 */
#ifndef FW_PIN_IRQ_MAX
#define FW_PIN_IRQ_MAX 8
#endif

/*
 * 引脚工作模式。
 *
 * 这些值会映射到 HAL 的 GPIO_MODE_*；数值本身没有硬件含义，
 * 新增模式时保持已有值不变，避免影响上层代码。
 */
#define FW_PIN_MODE_NONE 0       /* 未配置/复位状态 */
#define FW_PIN_MODE_OUTPUT 1     /* 推挽输出 */
#define FW_PIN_MODE_OUTPUT_OD 2  /* 开漏输出 */
#define FW_PIN_MODE_INPUT 3      /* 浮空输入 */
#define FW_PIN_MODE_INPUT_DOWN 4 /* 下拉输入 */
#define FW_PIN_MODE_INPUT_UP 5   /* 上拉输入 */
#define FW_PIN_MODE_ANALOG 6     /* 模拟输入（ADC/DAC） */

/*
 * ioctl 命令：设置引脚模式。
 *
 * 编码方式：高字节 0x08 表示“PIN 类配置命令”，低 24 位保留。
 * 当前 pin 类只支持这一条命令。
 */
#define FW_CTL_PIN_SET_MODE (0x08 << 24)

/* ioctl 参数结构。使用结构体而不是裸 int，便于以后扩展上下拉/速度等字段。 */
typedef struct {
    int mode; /* FW_PIN_MODE_* */
} fw_pin_config_t;

/* EXTI 触发沿。BOTH 是 RISING|FALLING 的位组合。 */
#define FW_PIN_IRQ_RISING  0x01
#define FW_PIN_IRQ_FALLING 0x02
#define FW_PIN_IRQ_BOTH    (FW_PIN_IRQ_RISING | FW_PIN_IRQ_FALLING)

/* 驱动向类上报的 ISR 事件；args 指向 int number。 */
#define FW_ISR_PIN_EDGE 0x01

/*
 * 引脚边沿回调。
 *
 * number - 触发中断的引脚编号（FW_PIN_NUMBER 编码）；
 * mode   - attach 时声明的 FW_PIN_IRQ_* 触发方式；
 * user   - 调用者私有指针。
 *
 * 回调在 ISR 上下文执行，禁止阻塞、禁止调用 rt_thread_mdelay 等 API；
 * 需要唤醒任务时，在回调里 rt_event_send / 释放信号量，把处理放到任务。
 */
typedef void (*fw_pin_irq_cb_t)(fw_dev_t *dev, int number, int mode,
                                void *user);

/* 类层的静态回调表项；驱动只负责硬件，不保存回调。 */
typedef struct {
    bool used;
    int number;
    int mode;
    fw_pin_irq_cb_t callback;
    void *user;
} fw_pin_irq_entry_t;

/*
 * pin 驱动操作集（第二层 ops）。
 *
 * 这些函数由 bsp 驱动实现，参数中的 number 已经由类从 offset 传入。
 *   configure  - 配置某个引脚的模式；
 *   read/write - 读/写电平；
 *   attach_irq - 让硬件开始监听某个引脚的边沿（驱动只需配置 EXTI）；
 *   detach_irq - 停止监听并释放 EXTI 线；
 *   enable_irq - 临时屏蔽/恢复某条线，不改变回调绑定。
 *
 * 新增函数追加在末尾，保持已有驱动/假设备的位置初始化顺序不变。
 */
typedef struct {
    int (*configure)(fw_dev_t *dev, int number, int mode);
    int (*read)(fw_dev_t *dev, int number);
    int (*write)(fw_dev_t *dev, int number, int value);
    int (*attach_irq)(fw_dev_t *dev, int number, int mode);
    int (*detach_irq)(fw_dev_t *dev, int number);
    int (*enable_irq)(fw_dev_t *dev, int number, bool enable);
} fw_pin_ops_t;

/*
 * pin 设备对象。
 *
 * dev 是基类；entries 是类层 IRQ 回调表，静态分配。驱动在 EXTI ISR 中
 * 通过 fw_dev_isr 上报引脚号，类在这里找到回调并执行。
 */
typedef struct {
    fw_dev_t dev;
    fw_pin_irq_entry_t entries[FW_PIN_IRQ_MAX];
} fw_pin_t;

/*
 * 引脚编号编码：高 4 位端口索引（0=A），低 4 位引脚号。
 *
 * 例如：
 *   PA5  -> (0 << 4) | 5  = 0x05
 *   PE2  -> (4 << 4) | 2  = 0x42
 *   PD13 -> (3 << 4) | 13 = 0x3D
 *
 * 这样编号可以直接作为 read/write 的 offset，驱动再用位运算拆出
 * 端口和引脚，不需要查大表。
 */
#define FW_PIN_NUMBER(port, pin) (((port) << 4) | ((pin) & 0x0f))

/*
 * 注册 pin 设备。
 * 参数 drv 的 type 必须是 FW_DEV_TYPE_PIN，ops 指向 fw_pin_ops_t。
 * 设备对象必须静态分配（通常由 bsp 驱动定义）。
 */
fw_status_t fw_pin_register(fw_pin_t *pin, const char *name, const fw_drv_t *drv);

/*
 * 配置引脚模式。
 * 内部走 fw_dev_ioctl(dev, number, FW_CTL_PIN_SET_MODE, &config)。
 * 返回 sizeof(config) 或负错误码。
 */
int fw_pin_configure(fw_dev_t *dev, int number, int mode);

/* 读取单个引脚电平，返回 0/1 或负错误码。 */
int fw_pin_read(fw_dev_t *dev, int number);

/* 写单个引脚电平；value 非 0 为高，0 为低。 */
int fw_pin_write(fw_dev_t *dev, int number, int value);

/*
 * 绑定/解绑边沿回调。
 *
 * attach 会占用一条静态表项和一条硬件 EXTI 线；同一条 EXTI 线（相同
 * 引脚号、不同端口）只能有一个 owner，冲突返回 FW_EBUSY。
 * attach 只配置 EXTI，不改变 GPIO 模式；调用前应先
 * fw_pin_configure(dev, number, FW_PIN_MODE_INPUT_UP/DOWN/INPUT)。
 * detach 会清除硬件监听并释放表项。
 * 回调在 ISR 上下文执行，必须非阻塞。
 */
fw_status_t fw_pin_attach_irq(fw_dev_t *dev, int number, int mode,
                              fw_pin_irq_cb_t callback, void *user);
fw_status_t fw_pin_detach_irq(fw_dev_t *dev, int number);
fw_status_t fw_pin_irq_enable(fw_dev_t *dev, int number, bool enable);

#endif
