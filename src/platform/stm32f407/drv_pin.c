/*
 * drv_pin.c - STM32 GPIO 单设备驱动。
 *
 * 层级：src/platform/stm32f407
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
 * 依赖：drv_pin.h、stm32f407_pin_map.h、stm32f407_map.h。
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
#include "board_config.h"
#include "stm32f407_pin_map.h"
#include "stm32f407_map.h"
#include <rthw.h>
#include <string.h>

/*
 * 编译期端口/引脚表。
 *
 * 两张表的长度分别对应“支持的端口数”和“每个端口的引脚数”；
 * 表内容来自 stm32f407_pin_map.h，板级移植时只需要改那张头文件。
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

#if defined(FW_USING_PIN_EXTI) && FW_USING_PIN_EXTI
/*
 * EXTI 线所有权表。
 *
 * 索引是引脚号 0..15（不是端口），值是端口索引 0=A..4=E；
 * 0xFF 表示该线空闲。STM32 的一条 EXTI 线只能属于一个端口，
 * 例如 PA5 和 PB5 不能同时设为中断源。
 */
static uint8_t s_exti_owner[16];

/* 16 条线 -> 7 个 NVIC 中断号，来自 stm32f407_pin_map.h。 */
static const IRQn_Type s_exti_irq[16] = DRV_PIN_IRQ_MAP_CONFIG;

/* EXTI 线 -> 引脚号的端口/引脚拆分辅助。 */
static bool exti_line_valid(int number)
{
    return number >= 0 && (number >> 4) <= 4 && (number & 0x0F) <= 15;
}

/* 清 s_exti_owner；由注册函数在调度器启动前调用一次。 */
static void exti_init(void)
{
    memset(s_exti_owner, 0xFF, sizeof(s_exti_owner));
}

/*
 * 判断同一个 NVIC 分组里是否还有其他 owner。
 *
 * EXTI5..9 共用一个 IRQ，EXTI10..15 共用另一个。detach 一条线时不能
 * 直接关闭整个分组中断，否则会误伤同组其他引脚。
 */
static bool exti_group_busy(IRQn_Type irq)
{
    size_t i;
    for (i = 0U; i < 16U; ++i) {
        if (s_exti_owner[i] != 0xFF && s_exti_irq[i] == irq) return true;
    }
    return false;
}

/*
 * 配置一条 EXTI 线。
 *
 * 步骤：
 *   1. SYSCFG.EXTICR 选择端口（每条线 4 bit）；
 *   2. EXTI.IMR 打开中断屏蔽；
 *   3. RTSR/FTSR 选择边沿；
 *   4. 写 PR 清掉配置前可能残留的 pending；
 *   5. 设置 NVIC 优先级并使能对应分组 IRQ。
 *
 * 同类 attach 重复调用时只更新边沿和 NVIC，不改变 owner；不同端口
 * 争用同一条线返回 FW_EBUSY。
 */
static int drv_pin_attach_irq(fw_dev_t *dev, int number, int mode)
{
    uint8_t port;
    uint8_t line;
    uint32_t bit;
    IRQn_Type irq;
    (void)dev;

    if (!exti_line_valid(number)) return FW_EINVAL;
    if (mode == 0 || (mode & ~FW_PIN_IRQ_BOTH) != 0) return FW_EINVAL;
    port = (uint8_t)(number >> 4);
    line = (uint8_t)(number & 0x0F);
    bit = 1U << line;

    if (s_exti_owner[line] != 0xFF && s_exti_owner[line] != port)
        return FW_EBUSY;

    __HAL_RCC_SYSCFG_CLK_ENABLE();
    SYSCFG->EXTICR[line >> 2U] &= ~(0x0FU << ((line & 0x03U) * 4U));
    SYSCFG->EXTICR[line >> 2U] |= (uint32_t)port << ((line & 0x03U) * 4U);

    EXTI->IMR |= bit;
    if ((mode & FW_PIN_IRQ_RISING) != 0) EXTI->RTSR |= bit;
    else EXTI->RTSR &= ~bit;
    if ((mode & FW_PIN_IRQ_FALLING) != 0) EXTI->FTSR |= bit;
    else EXTI->FTSR &= ~bit;
    EXTI->PR = bit;

    s_exti_owner[line] = port;
    irq = s_exti_irq[line];
    HAL_NVIC_SetPriority(irq, BOARD_EXTI_IRQ_PRIORITY, 0U);
    HAL_NVIC_EnableIRQ(irq);
    return FW_OK;
}

/* 停止监听一条线并释放 owner；同组还有其他线时保留 NVIC。 */
static int drv_pin_detach_irq(fw_dev_t *dev, int number)
{
    uint8_t line;
    uint32_t bit;
    IRQn_Type irq;
    (void)dev;

    if (!exti_line_valid(number)) return FW_EINVAL;
    line = (uint8_t)(number & 0x0F);
    bit = 1U << line;
    irq = s_exti_irq[line];

    EXTI->IMR &= ~bit;
    EXTI->RTSR &= ~bit;
    EXTI->FTSR &= ~bit;
    EXTI->PR = bit;
    s_exti_owner[line] = 0xFF;
    if (!exti_group_busy(irq)) HAL_NVIC_DisableIRQ(irq);
    return FW_OK;
}

/* 临时屏蔽/恢复一条线；不改变 owner 和边沿配置。 */
static int drv_pin_enable_irq(fw_dev_t *dev, int number, bool enable)
{
    uint8_t line;
    (void)dev;

    if (!exti_line_valid(number)) return FW_EINVAL;
    line = (uint8_t)(number & 0x0F);
    if (enable) {
        EXTI->PR = 1U << line; /* 清掉屏蔽期间可能积累的 pending */
        EXTI->IMR |= 1U << line;
    } else {
        EXTI->IMR &= ~(1U << line);
    }
    return FW_OK;
}

/*
 * 共享 EXTI 中断体。
 *
 * 逐位检查 pending，先写 1 清除，再根据 owner 计算完整引脚号，
 * 最后走 fw_dev_isr -> pin 类回调表。没有 owner 的 pending 也要清掉，
 * 防止错误配置导致中断风暴。
 */
static void exti_irq_handler(uint32_t mask)
{
    uint32_t pending = EXTI->PR & mask;
    uint8_t line;

    for (line = 0U; line < 16U; ++line) {
        uint32_t bit = 1U << line;
        if ((pending & bit) == 0U) continue;
        EXTI->PR = bit;
        if (s_exti_owner[line] != 0xFF) {
            int number = ((int)s_exti_owner[line] << 4) | (int)line;
            (void)fw_dev_isr(&pin_device.dev, FW_ISR_PIN_EDGE, &number);
        }
    }
}

/* EXTI0..4 各自独立；EXTI5..9 共用 EXTI9_5；EXTI10..15 共用 EXTI15_10。 */
void EXTI0_IRQHandler(void)
{
    rt_interrupt_enter();
    exti_irq_handler(1U << 0);
    rt_interrupt_leave();
}

void EXTI1_IRQHandler(void)
{
    rt_interrupt_enter();
    exti_irq_handler(1U << 1);
    rt_interrupt_leave();
}

void EXTI2_IRQHandler(void)
{
    rt_interrupt_enter();
    exti_irq_handler(1U << 2);
    rt_interrupt_leave();
}

void EXTI3_IRQHandler(void)
{
    rt_interrupt_enter();
    exti_irq_handler(1U << 3);
    rt_interrupt_leave();
}

void EXTI4_IRQHandler(void)
{
    rt_interrupt_enter();
    exti_irq_handler(1U << 4);
    rt_interrupt_leave();
}

void EXTI9_5_IRQHandler(void)
{
    rt_interrupt_enter();
    exti_irq_handler(0x03E0U); /* bits 5..9 */
    rt_interrupt_leave();
}

void EXTI15_10_IRQHandler(void)
{
    rt_interrupt_enter();
    exti_irq_handler(0xFC00U); /* bits 10..15 */
    rt_interrupt_leave();
}
#endif /* FW_USING_PIN_EXTI */

/* pin 驱动 ops：类层会把 offset 转换成 number 后调用这些函数。 */
static const fw_pin_ops_t drv_pin_ops = {
    drv_pin_configure,
    drv_pin_read,
    drv_pin_write,
#if defined(FW_USING_PIN_EXTI) && FW_USING_PIN_EXTI
    drv_pin_attach_irq,
    drv_pin_detach_irq,
    drv_pin_enable_irq
#else
    NULL,
    NULL,
    NULL
#endif
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
#if defined(FW_USING_PIN_EXTI) && FW_USING_PIN_EXTI
    exti_init();
#endif
    return fw_pin_register(&pin_device, "pin", &drv_pin_drv);
}
