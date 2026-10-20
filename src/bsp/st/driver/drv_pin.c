/*
 * drv_pin.c - STM32 GPIO 单设备驱动。
 *
 * 层级：src/bsp/st/driver
 * 职责：
 *   1. 把引脚号解析成端口 + 引脚掩码；
 *   2. 按模式配置 HAL GPIO；
 *   3. 读写单个引脚电平；
 *   4. 注册唯一的 "pin" 设备。
 *
 * 上下文：
 *   - configure 调用 HAL_GPIO_Init，只在任务上下文使用；
 *   - read/write 只访问 IDR/BSRR 寄存器，可在 ISR 中使用。
 *
 * 依赖：drv_pin.h、mr_board.h、stm32f407_map.h。
 *
 * 引脚号编码（与 fw/pin.h 一致）：
 *
 *   bit7..4 端口索引 0=A,1=B,...   bit3..0 引脚号 0..15
 *
 * 例如 PE2 = (4 << 4) | 2 = 0x42。
 * 驱动用 number >> 4 取端口，用 number & 0x0F 取引脚掩码下标。
 *
 * 为什么不在驱动里为每个引脚建对象：
 *   GPIO 的操作只依赖端口寄存器和掩码，没有每引脚独立状态；
 *   一个设备 + 编号已经足够，能显著减少静态 RAM 和注册代码。
 */
#include "drv_pin.h"
#include "mr_board.h"
#include "stm32f407_map.h"

/*
 * 编译期端口/引脚表。
 *
 * 两张表的长度分别对应“支持的端口数”和“每个端口的引脚数”；
 * 表内容来自 mr_board.h，板级移植时只需要改那张头文件。
 */
static GPIO_TypeDef *const pin_ports[] = DRV_PIN_PORT_CONFIG;
static const uint16_t pin_pins[] = DRV_PIN_CONFIG;

/* 唯一设备对象；放在 .bss，生命周期覆盖整个程序。 */
static fw_pin_t pin_device;

/*
 * 从引脚号取出端口指针。
 *
 * index = (number >> 4) & 0x0F：
 *   高位是端口索引；&0x0F 防止调用者传入负数或越界值时读到表外。
 * 端口索引超出表长时返回 NULL，由调用者转换成 FW_EINVAL。
 */
static GPIO_TypeDef *pin_port(int number)
{
    int index = (number >> 4) & 0x0f;
    if (index < 0 || index >= (int)(sizeof(pin_ports) / sizeof(pin_ports[0])))
        return NULL;
    return pin_ports[index];
}

/*
 * 从引脚号取出 HAL 引脚掩码。
 *
 * index = number & 0x0F；表长固定为 16。
 * 返回 0 表示非法引脚号（调用者据此报错）。
 * 注意 GPIO_PIN_0 的值是 1，不是 0，所以掩码 0 可以安全地表示错误。
 */
static uint16_t pin_mask(int number)
{
    int index = number & 0x0f;
    if (index < 0 || index >= (int)(sizeof(pin_pins) / sizeof(pin_pins[0])))
        return 0U;
    return pin_pins[index];
}

/*
 * 配置引脚模式。
 *
 * 步骤：
 *   1. 解析端口和掩码，拒绝非法引脚号；
 *   2. 把框架的 FW_PIN_MODE_* 映射到 HAL 的 GPIO_MODE_*；
 *   3. 使能对应端口时钟；
 *   4. 调用 HAL_GPIO_Init。
 *
 * 速度统一设为 HIGH：GPIO 输出用于 LED/片选/背光，频率要求不高；
 * 如果是高速信号，应在具体驱动中单独设置。
 *
 * NONE 与 INPUT 都映射为浮空输入：NONE 表示“未配置/复位”，
 * 但引脚仍需要一个确定的 HAL 模式，浮空输入是最安全的默认值。
 */
static int drv_pin_configure(fw_dev_t *dev, int number, int mode)
{
    GPIO_InitTypeDef init = {0};
    GPIO_TypeDef *port = pin_port(number);
    uint16_t mask = pin_mask(number);
    (void)dev; /* 只有一个 pin 设备，驱动状态与 dev 无关 */

    if (port == NULL || mask == 0U || number < 0) return FW_EINVAL;

    switch (mode) {
    case FW_PIN_MODE_NONE:
    case FW_PIN_MODE_INPUT:
        init.Mode = GPIO_MODE_INPUT;
        init.Pull = GPIO_NOPULL;
        break;
    case FW_PIN_MODE_INPUT_DOWN:
        init.Mode = GPIO_MODE_INPUT;
        init.Pull = GPIO_PULLDOWN;
        break;
    case FW_PIN_MODE_INPUT_UP:
        init.Mode = GPIO_MODE_INPUT;
        init.Pull = GPIO_PULLUP;
        break;
    case FW_PIN_MODE_OUTPUT:
        init.Mode = GPIO_MODE_OUTPUT_PP;
        init.Pull = GPIO_NOPULL;
        break;
    case FW_PIN_MODE_OUTPUT_OD:
        init.Mode = GPIO_MODE_OUTPUT_OD;
        init.Pull = GPIO_NOPULL;
        break;
    case FW_PIN_MODE_ANALOG:
        init.Mode = GPIO_MODE_ANALOG;
        init.Pull = GPIO_NOPULL;
        break;
    default:
        return FW_EINVAL;
    }

    /* 配置前必须打开端口时钟，否则寄存器写入无效。 */
    if (stm32f407_gpio_clock_enable((uint8_t)(number >> 4)) != FW_OK)
        return FW_EINVAL;

    init.Pin = mask;
    init.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(port, &init);
    return FW_OK;
}

/*
 * 读取引脚电平。
 * HAL_GPIO_ReadPin 返回 GPIO_PinState，这里归一化成 0/1。
 * 只读 IDR，ISR 安全。
 */
static int drv_pin_read(fw_dev_t *dev, int number)
{
    GPIO_TypeDef *port = pin_port(number);
    uint16_t mask = pin_mask(number);
    (void)dev;
    if (port == NULL || mask == 0U) return FW_EINVAL;
    return HAL_GPIO_ReadPin(port, mask) == GPIO_PIN_SET ? 1 : 0;
}

/*
 * 写引脚电平。
 * HAL_GPIO_WritePin 使用 BSRR 寄存器，写入是原子的，
 * 不会像“读-改-写”那样影响同端口的其他引脚，因此 ISR 安全。
 */
static int drv_pin_write(fw_dev_t *dev, int number, int value)
{
    GPIO_TypeDef *port = pin_port(number);
    uint16_t mask = pin_mask(number);
    (void)dev;
    if (port == NULL || mask == 0U) return FW_EINVAL;
    HAL_GPIO_WritePin(port, mask, value != 0 ? GPIO_PIN_SET : GPIO_PIN_RESET);
    return FW_OK;
}

/* pin 驱动 ops：类层会把 offset 转换成 number 后调用这些函数。 */
static const fw_pin_ops_t drv_pin_ops = {
    drv_pin_configure,
    drv_pin_read,
    drv_pin_write
};

/*
 * 驱动描述。
 * type 必须与 pin 类匹配；data 为 NULL，因为所有状态都在静态表里。
 */
static const fw_drv_t drv_pin_drv = {
    FW_DEV_TYPE_PIN,
    &drv_pin_ops,
    NULL
};

/*
 * 注册 pin 设备。
 *
 * 只注册一次 "pin"；返回 core 的错误码（重复注册为 FW_EEXIST）。
 * 这个函数通常由 board_devices_init() 第一步调用。
 */
fw_status_t drv_pin_register(void)
{
    return fw_pin_register(&pin_device, "pin", &drv_pin_drv);
}
