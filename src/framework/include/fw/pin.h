/*
 * fw/pin.h - mr 风格 GPIO 设备类。
 *
 * 层级：src/framework/core
 * 职责：
 *   1. 用一个名为 "pin" 的设备覆盖芯片所有 GPIO；
 *   2. 用 offset（引脚号）区分具体引脚，而不是为每个引脚创建对象；
 *   3. 提供读电平、写电平、切换输入/输出/模拟模式三个动作。
 *
 * 上下文：
 *   - read/write 只访问寄存器，可在任务或 ISR 中调用；
 *   - configure 会调用 HAL_GPIO_Init，只在任务上下文调用。
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

#include "fw/dev.h"

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

/*
 * pin 驱动操作集（第二层 ops）。
 *
 * 这些函数由 bsp 驱动实现，参数中的 number 已经由类从 offset 传入。
 *   configure - 配置某个引脚的模式；
 *   read      - 返回 0/1 电平，失败返回负错误码；
 *   write     - value 非 0 输出高电平，0 输出低电平。
 */
typedef struct {
    int (*configure)(fw_dev_t *dev, int number, int mode);
    int (*read)(fw_dev_t *dev, int number);
    int (*write)(fw_dev_t *dev, int number, int value);
} fw_pin_ops_t;

/*
 * pin 设备对象。
 *
 * 只有一个 fw_dev_t 基类成员，不需要额外状态：
 * 引脚号通过 read/write 的 offset 传入，硬件映射由驱动查表完成。
 */
typedef struct {
    fw_dev_t dev;
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

#endif
