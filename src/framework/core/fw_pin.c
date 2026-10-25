/*
 * fw_pin.c - pin 设备类的通用实现。
 *
 * 层级：src/framework/core
 * 职责：
 *   1. 把 fw_dev_read/write/ioctl 的 offset 解释为引脚号；
 *   2. 把单引脚动作拆成驱动 ops 调用；
 *   3. 支持一次读写多个连续编号的引脚。
 *
 * 上下文：
 *   - read/write 只做循环和转发，可在 ISR 中使用；
 *   - configure 最终调用 HAL_GPIO_Init，只能在任务上下文使用。
 *
 * 依赖：fw/pin.h。
 *
 * 设计说明：
 *   类层不接触 GPIO 寄存器，也不包含任何 HAL 头文件；
 *   端口表、引脚掩码和时钟使能全部由 bsp 驱动负责。
 *   这样更换芯片时只需要改驱动，pin 类可以原样复用。
 */
#include "fw/pin.h"

/* 从设备基类取出驱动 ops；任何一层为空都返回 NULL，避免空指针调用。 */
static const fw_pin_ops_t *pin_ops(const fw_dev_t *dev)
{
    if (dev == NULL || dev->drv == NULL || dev->drv->ops == NULL) return NULL;
    return (const fw_pin_ops_t *)dev->drv->ops;
}

/*
 * 读多个引脚。
 *
 * offset 是起始引脚号，size 是连续编号的个数。
 * 每个引脚的结果被压缩成一个字节（0 或 1）写入 buf，
 * 这样调用者可以用一个字节数组读取一段连续编号的引脚。
 *
 * 注意：这里的“连续”指编号连续，不保证物理相邻；
 * 例如 0x4F 的下一个是 0x50，会跨端口。
 *
 * 返回实际读取的字节数；任一步失败立即返回负错误码。
 */
static int pin_read(fw_dev_t *dev, int off, void *buf, size_t size,
                    uint32_t timeout_ms)
{
    const fw_pin_ops_t *ops = pin_ops(dev);
    uint8_t *bytes = (uint8_t *)buf;
    size_t i;
    (void)timeout_ms; /* 引脚读取是纯寄存器操作，没有超时概念 */

    if (ops == NULL || ops->read == NULL || buf == NULL) return FW_EINVAL;
    for (i = 0U; i < size; ++i) {
        int value = ops->read(dev, off + (int)i);
        if (value < 0) return value;
        bytes[i] = (uint8_t)(value != 0);
    }
    return (int)size;
}

/*
 * 写多个引脚。
 *
 * buf 中每个字节非 0 表示高电平，0 表示低电平。
 * 返回实际写入的字节数；任一步失败立即返回负错误码。
 */
static int pin_write(fw_dev_t *dev, int off, const void *buf, size_t size,
                     uint32_t timeout_ms)
{
    const fw_pin_ops_t *ops = pin_ops(dev);
    const uint8_t *bytes = (const uint8_t *)buf;
    size_t i;
    (void)timeout_ms;

    if (ops == NULL || ops->write == NULL || buf == NULL) return FW_EINVAL;
    for (i = 0U; i < size; ++i) {
        int result = ops->write(dev, off + (int)i, bytes[i]);
        if (result < 0) return result;
    }
    return (int)size;
}

/*
 * 引脚控制。
 *
 * 当前只实现 FW_CTL_PIN_SET_MODE：
 *   args 指向 fw_pin_config_t，内部只取 mode 字段；
 *   驱动 execute 成功后返回 sizeof(config)，方便调用者检查 ABI 兼容性。
 *
 * 其他命令返回 FW_ENOTSUP，调用者可以据此判断功能是否可用。
 */
static int pin_ioctl(fw_dev_t *dev, int off, int cmd, void *args)
{
    const fw_pin_ops_t *ops = pin_ops(dev);
    if (ops == NULL || ops->configure == NULL) return FW_ENOTSUP;

    if (cmd == FW_CTL_PIN_SET_MODE) {
        fw_pin_config_t config;
        if (args == NULL) return FW_EINVAL;
        config = *(const fw_pin_config_t *)args;
        {
            int result = ops->configure(dev, off, config.mode);
            if (result < 0) return result;
        }
        return (int)sizeof(config);
    }
    return FW_ENOTSUP;
}

/*
 * EXTI 事件入口（由驱动 ISR 调用）。
 *
 * 驱动只负责把“哪条引脚线触发了”转换成引脚编号；类层负责查静态回调表。
 * 这样驱动不需要知道应用回调，pin 类也不接触 EXTI 寄存器。
 *
 * args 指向 int number（引脚编号）。回调在 ISR 上下文执行，必须非阻塞。
 * 找不到对应表项时返回 FW_ENOTREADY，便于诊断“硬件已使能但没有绑定回调”。
 */
static int pin_isr(fw_dev_t *dev, int event, void *args)
{
    fw_pin_t *pin;
    int number;
    size_t i;

    if (dev == NULL || args == NULL || event != FW_ISR_PIN_EDGE)
        return FW_ENOTSUP;
    number = *(const int *)args;
    pin = FW_CONTAINER_OF(dev, fw_pin_t, dev);

    for (i = 0U; i < FW_PIN_IRQ_MAX; ++i) {
        if (pin->entries[i].used && pin->entries[i].number == number) {
            if (pin->entries[i].callback != NULL)
                pin->entries[i].callback(dev, number, pin->entries[i].mode,
                                         pin->entries[i].user);
            return FW_OK;
        }
    }
    return FW_ENOTREADY;
}

/*
 * pin 类操作表。
 *
 * open/close 只做引用计数；read/write/ioctl 是任务或 ISR 可用的寄存器操作；
 * isr 由驱动在 EXTI 中断中调用。
 */
static const fw_dev_ops_t pin_dev_ops = {
    NULL,
    NULL,
    pin_read,
    pin_write,
    pin_ioctl,
    pin_isr
};

/*
 * 注册 pin 设备。
 *
 * 校验：
 *   - 对象和名字非空；
 *   - 驱动类型必须是 FW_DEV_TYPE_PIN；
 *   - 注册到 core 成功后才挂接类 ops。
 *
 * 一个系统通常只注册一次 "pin"；重复注册会被 core 以 FW_EEXIST 拒绝。
 */
fw_status_t fw_pin_register(fw_pin_t *pin, const char *name, const fw_drv_t *drv)
{
    fw_status_t result;
    if (pin == NULL || name == NULL || drv == NULL || drv->ops == NULL)
        return FW_EINVAL;
    if (drv->type != FW_DEV_TYPE_PIN) return FW_EINVAL;

    result = fw_dev_register(&pin->dev, name, FW_DEV_TYPE_PIN, drv);
    if (result != FW_OK) return result;

    pin->dev.ops = &pin_dev_ops;
    return FW_OK;
}

/*
 * 配置引脚模式的便捷函数。
 * 把 mode 装进 fw_pin_config_t，再走统一 ioctl 通道。
 */
int fw_pin_configure(fw_dev_t *dev, int number, int mode)
{
    fw_pin_config_t config;
    config.mode = mode;
    return fw_dev_ioctl(dev, number, FW_CTL_PIN_SET_MODE, &config);
}

/*
 * 读单个引脚。
 * 用一字节缓冲调用通用 read，再把结果转换成 int 返回。
 */
int fw_pin_read(fw_dev_t *dev, int number)
{
    uint8_t value = 0U;
    int result = fw_dev_read(dev, number, &value, 1U, 0U);
    return result < 0 ? result : (int)value;
}

/*
 * 写单个引脚。
 * 把任意非 0 值归一化为 1，避免把 0x55 之类的值直接写进缓冲。
 */
int fw_pin_write(fw_dev_t *dev, int number, int value)
{
    uint8_t byte = value != 0 ? 1U : 0U;
    return fw_dev_write(dev, number, &byte, 1U, 0U);
}

/*
 * 绑定一个引脚边沿回调。
 *
 * 顺序：
 *   1. 校验类型、回调、触发模式；
 *   2. 先查同编号的旧表项（允许重复 attach 更新回调）；
 *   3. 没有旧项就找空槽；满了返回 FW_EOVERFLOW；
 *   4. 先写类层表项，再调用驱动配置硬件；
 *      驱动失败时回滚表项，避免“软件以为绑定了但硬件没使能”。
 *
 * 注意：驱动负责 EXTI 线唯一性；同一个引脚号在不同端口上共享一条
 * EXTI 线，冲突由驱动返回 FW_EBUSY。
 */
fw_status_t fw_pin_attach_irq(fw_dev_t *dev, int number, int mode,
                              fw_pin_irq_cb_t callback, void *user)
{
    const fw_pin_ops_t *ops = pin_ops(dev);
    fw_pin_t *pin;
    size_t slot;
    size_t i = 0U;
    bool existed = false;

    if (dev == NULL || dev->type != FW_DEV_TYPE_PIN || callback == NULL ||
        (mode & ~FW_PIN_IRQ_BOTH) != 0 || mode == 0 ||
        ops == NULL || ops->attach_irq == NULL) return FW_ENOTSUP;
    pin = FW_CONTAINER_OF(dev, fw_pin_t, dev);

    for (i = 0U; i < FW_PIN_IRQ_MAX; ++i) {
        if (pin->entries[i].used && pin->entries[i].number == number) {
            existed = true;
            break;
        }
    }
    if (existed) {
        slot = i;
    } else {
        for (slot = 0U; slot < FW_PIN_IRQ_MAX; ++slot) {
            if (!pin->entries[slot].used) break;
        }
        if (slot >= FW_PIN_IRQ_MAX) return FW_EOVERFLOW;
    }

    /* 先登记软件回调，硬件 attach 失败时再回滚。 */
    pin->entries[slot].used = true;
    pin->entries[slot].number = number;
    pin->entries[slot].mode = mode;
    pin->entries[slot].callback = callback;
    pin->entries[slot].user = user;

    {
        int result = ops->attach_irq(dev, number, mode);
        if (result < 0) {
            pin->entries[slot].used = false;
            pin->entries[slot].callback = NULL;
            pin->entries[slot].user = NULL;
            return (fw_status_t)result;
        }
    }
    return FW_OK;
}

/* 解绑并释放表项；硬件 detach 失败也清软件表，避免回调悬空。 */
fw_status_t fw_pin_detach_irq(fw_dev_t *dev, int number)
{
    const fw_pin_ops_t *ops = pin_ops(dev);
    fw_pin_t *pin;
    size_t i;

    if (dev == NULL || dev->type != FW_DEV_TYPE_PIN || ops == NULL ||
        ops->detach_irq == NULL) return FW_ENOTSUP;
    pin = FW_CONTAINER_OF(dev, fw_pin_t, dev);

    for (i = 0U; i < FW_PIN_IRQ_MAX; ++i) {
        if (!pin->entries[i].used || pin->entries[i].number != number)
            continue;
        pin->entries[i].used = false;
        pin->entries[i].callback = NULL;
        pin->entries[i].user = NULL;
        {
            int result = ops->detach_irq(dev, number);
            return result < 0 ? (fw_status_t)result : FW_OK;
        }
    }
    return FW_ENOTREADY;
}

/* 临时屏蔽/恢复 EXTI 线，不改变回调绑定。 */
fw_status_t fw_pin_irq_enable(fw_dev_t *dev, int number, bool enable)
{
    const fw_pin_ops_t *ops = pin_ops(dev);
    fw_pin_t *pin;
    size_t i;

    if (dev == NULL || dev->type != FW_DEV_TYPE_PIN || ops == NULL ||
        ops->enable_irq == NULL) return FW_ENOTSUP;
    pin = FW_CONTAINER_OF(dev, fw_pin_t, dev);
    for (i = 0U; i < FW_PIN_IRQ_MAX; ++i) {
        if (!pin->entries[i].used || pin->entries[i].number != number)
            continue;
        {
            int result = ops->enable_irq(dev, number, enable);
            return result < 0 ? (fw_status_t)result : FW_OK;
        }
    }
    return FW_ENOTREADY;
}
