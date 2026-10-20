/*
 * serial.c - serial 设备类的通用实现。
 *
 * 层级：src/framework/core
 * 职责：
 *   1. 维护每个串口的 RX/TX ring；
 *   2. 实现阻塞/非阻塞 read、非阻塞 write；
 *   3. 处理配置、清缓冲、查询缓冲深度等 ioctl；
 *   4. 把驱动上报的中断事件转换成 ring 操作和错误状态。
 *
 * 上下文：
 *   - open/close/read/write/ioctl/wait_rx：任务上下文；
 *   - isr：由 bsp 驱动在 USART 中断中调用，必须非阻塞。
 *
 * 依赖：fw/serial.h。
 *
 * 并发要点：
 *   任务与 ISR 共享 rd_fifo/wr_fifo。类不直接关中断，而是调用
 *   驱动注入的 lock/unlock；当前 STM32 驱动用 PRIMASK 实现，
 *   因此临界区可以同时保护任务侧和中断侧。
 *   这样 framework/core 不依赖 RT-Thread，移植层可以换成其他实现。
 */
#include "fw/serial.h"

/*
 * 默认配置：115200-8-N-1。
 * 板级注册时通常会传入自己的配置；只有调用者传 NULL 时才使用这里。
 */
static const fw_serial_config_t default_config = {
    115200U, FW_SERIAL_DATA_BITS_8, FW_SERIAL_STOP_BITS_1, FW_SERIAL_PARITY_NONE
};

/* 从基类取驱动 ops，任何一层为空都返回 NULL。 */
static const fw_serial_ops_t *serial_ops(const fw_dev_t *dev)
{
    if (dev == NULL || dev->drv == NULL || dev->drv->ops == NULL) return NULL;
    return (const fw_serial_ops_t *)dev->drv->ops;
}

/*
 * 进入临界区。
 *
 * 返回值是平台相关的“中断级别”，必须原样传给 serial_unlock。
 * 如果驱动没有实现 lock（例如 Host 假设备），返回 0，表示无需恢复。
 */
static int serial_lock(fw_dev_t *dev)
{
    const fw_serial_ops_t *ops = serial_ops(dev);
    return ops != NULL && ops->lock != NULL ? ops->lock(dev) : 0;
}

/* 退出临界区；与 serial_lock 成对使用。 */
static void serial_unlock(fw_dev_t *dev, int level)
{
    const fw_serial_ops_t *ops = serial_ops(dev);
    if (ops != NULL && ops->unlock != NULL) ops->unlock(dev, level);
}

/*
 * 打开串口。
 *
 * 行为：
 *   1. 清空 RX/TX ring，丢弃上一次使用留下的残留数据；
 *   2. 清除历史错误；
 *   3. 把软件配置下发给驱动（HAL_UART_Init）。
 *
 * 清缓冲和 configure 都放在锁内，避免清空的同时 ISR 又写入一个字节。
 * 注意：serial_lock 内部只是关中断，不能在里面调用可能阻塞的 HAL 函数；
 * 当前 configure 在锁外调用，正是为了避免长时间关中断。
 */
static fw_status_t serial_open(fw_dev_t *dev)
{
    fw_serial_t *serial = FW_CONTAINER_OF(dev, fw_serial_t, dev);
    const fw_serial_ops_t *ops = serial_ops(dev);
    int level = serial_lock(dev);

    fw_ring_clear(&serial->rd_fifo);
    fw_ring_clear(&serial->wr_fifo);
    serial->error = FW_OK;
    serial_unlock(dev, level);

    if (ops != NULL && ops->configure != NULL) {
        int result = ops->configure(dev, &serial->config);
        return result < 0 ? (fw_status_t)result : FW_OK;
    }
    return FW_OK;
}

/*
 * 关闭串口。
 *
 * 当前只清空缓冲和错误状态，不关闭 USART 外设：
 * 控制台串口需要一直保持可输出，重复 open/close 也不应影响硬件。
 * 如果以后需要低功耗，可以在 ref_count 归零时由驱动关闭时钟。
 */
static fw_status_t serial_close(fw_dev_t *dev)
{
    fw_serial_t *serial = FW_CONTAINER_OF(dev, fw_serial_t, dev);
    int level = serial_lock(dev);

    fw_ring_clear(&serial->rd_fifo);
    fw_ring_clear(&serial->wr_fifo);
    serial->error = FW_OK;
    serial_unlock(dev, level);
    return FW_OK;
}

/*
 * 读取数据。
 *
 * 逻辑是一个循环：
 *   1. 先在锁内检查错误状态；有错误则上报一次并清空 RX ring；
 *   2. 如果 ring 里有数据，拷贝最多 size 字节并返回实际数量；
 *   3. ring 为空且 timeout_ms==0，立即返回 0（非阻塞）；
 *   4. 否则调用驱动 wait_rx 等待事件，超时返回 0，然后重新检查 ring。
 *
 * 为什么 wait_rx 返回后要重新循环：
 *   事件可能在“检查 ring”和“进入等待”之间被 ISR 发送，
 *   驱动 wait_rx 内部也会先检查一次状态，避免丢失唤醒。
 *
 * 返回值：
 *   >0  实际读到的字节数；
 *   0   非阻塞无数据或等待超时；
 *   <0  fw_status_t 错误码。
 *
 * 注意：超时返回 0 而不是 FW_ETIMEOUT，是串口 read 的常见约定，
 * 让上层可以把它当作“暂时没有数据”处理。
 */
static int serial_read(fw_dev_t *dev, int off, void *buf, size_t size,
                       uint32_t timeout_ms)
{
    fw_serial_t *serial = FW_CONTAINER_OF(dev, fw_serial_t, dev);
    const fw_serial_ops_t *ops = serial_ops(dev);
    (void)off; /* serial 没有通道偏移，所有数据走同一个 ring */

    if (buf == NULL && size != 0U) return FW_EINVAL;
    if (size == 0U) return 0;

    for (;;) {
        int level = serial_lock(dev);

        /* 错误优先于数据：先上报，避免应用一直读到旧数据而忽略硬件错误。 */
        if (serial->error != FW_OK) {
            fw_status_t error = serial->error;
            serial->error = FW_OK;
            fw_ring_clear(&serial->rd_fifo);
            serial_unlock(dev, level);
            return (int)error;
        }

        if (serial->rd_fifo.count != 0U) {
            size_t count = fw_ring_read(&serial->rd_fifo, (uint8_t *)buf, size);
            serial_unlock(dev, level);
            return (int)count;
        }
        serial_unlock(dev, level);

        /* 非阻塞模式：没有数据就立即返回 0。 */
        if (timeout_ms == 0U) return 0;

        /* 有等待能力就睡在驱动的事件上；没有则退化为轮询一次。 */
        if (ops != NULL && ops->wait_rx != NULL) {
            fw_status_t result = ops->wait_rx(dev, timeout_ms);
            if (result == FW_ETIMEOUT) return 0;
            if (result != FW_OK) return (int)result;
        }
    }
}

/*
 * 写入数据。
 *
 * 这里只负责把数据放进 TX ring，不等待发送完成：
 *   - 返回实际放进 ring 的字节数，可能小于 size（短写）；
 *   - 只要放入了数据，就调用 start_tx 打开 TXE 中断；
 *   - 实际逐字节发送由 ISR 中的 FW_ISR_SERIAL_WR 分支完成。
 *
 * 为什么不在 write 里直接等发送完成：
 *   控制台输出和协议回复都不希望阻塞主循环；
 *   非阻塞 + 短写语义可以让上层自己决定重试策略。
 *
 * timeout_ms 当前未使用：ring 满时立即短写返回，由调用者稍后重试。
 */
static int serial_write(fw_dev_t *dev, int off, const void *buf, size_t size,
                        uint32_t timeout_ms)
{
    fw_serial_t *serial = FW_CONTAINER_OF(dev, fw_serial_t, dev);
    const fw_serial_ops_t *ops = serial_ops(dev);
    size_t count;
    int level;
    (void)off;
    (void)timeout_ms;

    if (buf == NULL && size != 0U) return FW_EINVAL;
    if (size == 0U) return 0;

    /* ring 的任务侧写操作必须和 ISR 的读操作互斥。 */
    level = serial_lock(dev);
    count = fw_ring_write(&serial->wr_fifo, (const uint8_t *)buf, size);
    serial_unlock(dev, level);

    /* 只有确实写入了数据才需要启动发送，避免空 ring 时反复开中断。 */
    if (count != 0U && ops != NULL && ops->start_tx != NULL) {
        (void)ops->start_tx(dev);
    }
    return (int)count;
}

/*
 * 串口控制接口。
 *
 * SET_CONFIG:
 *   - 校验波特率、数据位、停止位、校验位；
 *   - 先调用驱动 configure，成功后才更新软件副本；
 *   - 返回 sizeof(config)，便于调用者检查参数结构体版本。
 *
 * GET_CONFIG:
 *   直接返回软件副本，不访问硬件。
 *
 * CLR_RD_BUF / CLR_WR_BUF:
 *   在临界区内清空对应 ring；不会关闭中断源。
 *
 * GET_RD_DATASZ / GET_WR_DATASZ:
 *   在临界区内读取 count，结果写入调用者的 size_t。
 */
static int serial_ioctl(fw_dev_t *dev, int off, int cmd, void *args)
{
    fw_serial_t *serial = FW_CONTAINER_OF(dev, fw_serial_t, dev);
    const fw_serial_ops_t *ops = serial_ops(dev);
    (void)off;

    switch (cmd) {
    case FW_CTL_SERIAL_SET_CONFIG: {
        fw_serial_config_t config;
        if (args == NULL || ops == NULL || ops->configure == NULL)
            return FW_EINVAL;
        config = *(const fw_serial_config_t *)args;

        /* 只接受驱动能够表达的配置组合，避免“设置成功但硬件被忽略”。 */
        if (config.baud == 0U || config.data_bits != FW_SERIAL_DATA_BITS_8 ||
            (config.stop_bits != FW_SERIAL_STOP_BITS_1 &&
             config.stop_bits != FW_SERIAL_STOP_BITS_2) ||
            config.parity < FW_SERIAL_PARITY_NONE ||
            config.parity > FW_SERIAL_PARITY_ODD) return FW_EINVAL;
        {
            int result = ops->configure(dev, &config);
            if (result < 0) return result;
        }
        serial->config = config;
        return (int)sizeof(config);
    }
    case FW_CTL_SERIAL_GET_CONFIG:
        if (args == NULL) return FW_EINVAL;
        *(fw_serial_config_t *)args = serial->config;
        return (int)sizeof(serial->config);
    case FW_CTL_SERIAL_CLR_RD_BUF: {
        int level = serial_lock(dev);
        fw_ring_clear(&serial->rd_fifo);
        serial_unlock(dev, level);
        return FW_OK;
    }
    case FW_CTL_SERIAL_CLR_WR_BUF: {
        int level = serial_lock(dev);
        fw_ring_clear(&serial->wr_fifo);
        serial_unlock(dev, level);
        return FW_OK;
    }
    case FW_CTL_SERIAL_GET_RD_DATASZ: {
        int level;
        if (args == NULL) return FW_EINVAL;
        level = serial_lock(dev);
        *(size_t *)args = serial->rd_fifo.count;
        serial_unlock(dev, level);
        return (int)sizeof(size_t);
    }
    case FW_CTL_SERIAL_GET_WR_DATASZ: {
        int level;
        if (args == NULL) return FW_EINVAL;
        level = serial_lock(dev);
        *(size_t *)args = serial->wr_fifo.count;
        serial_unlock(dev, level);
        return (int)sizeof(size_t);
    }
    default:
        return FW_ENOTSUP;
    }
}

/*
 * 中断事件处理（由驱动在 USART IRQ 中调用）。
 *
 * RD（收到字节）：
 *   - 调用驱动 read_byte 读 DR；
 *   - 写入 rd_fifo；
 *   - ring 满时把 error 标记为 FW_EOVERFLOW，下一次 read 会上报。
 *
 * WR（发送寄存器空）：
 *   - 从 wr_fifo 取一个字节写 DR；
 *   - ring 已空则调用 stop_tx 关闭 TXE，发送结束。
 *
 * ERR（硬件错误）：
 *   - 记录 FW_EOVERFLOW，下一次 read 上报并清空 ring。
 *   - 具体错误类型（ORE/FE/NE/PE）由驱动读取并清除标志，
 *     类层只关心“有错误需要让应用知道”。
 *
 * 注意：这些函数在中断上下文执行，不能调用任何阻塞 API。
 * 当前实现只操作 ring 和标志，满足要求。
 */
static int serial_isr(fw_dev_t *dev, int event, void *args)
{
    fw_serial_t *serial = FW_CONTAINER_OF(dev, fw_serial_t, dev);
    const fw_serial_ops_t *ops = serial_ops(dev);
    (void)args;
    if (ops == NULL) return FW_ENOTSUP;

    if (event == FW_ISR_SERIAL_RD && ops->read_byte != NULL) {
        int byte = ops->read_byte(dev);
        if (byte < 0) return byte;
        {
            uint8_t value = (uint8_t)byte;
            /* ring 满说明应用消费太慢；保留旧数据并记录溢出。 */
            if (fw_ring_write(&serial->rd_fifo, &value, 1U) != 1U) {
                serial->error = FW_EOVERFLOW;
            }
        }
        return (int)serial->rd_fifo.count;
    }

    if (event == FW_ISR_SERIAL_WR && ops->write_byte != NULL) {
        uint8_t value;
        if (fw_ring_read(&serial->wr_fifo, &value, 1U) == 1U) {
            (void)ops->write_byte(dev, value);
        } else if (ops->stop_tx != NULL) {
            /* 没有更多数据：关闭 TXE，避免中断持续触发。 */
            (void)ops->stop_tx(dev);
        }
        return (int)serial->wr_fifo.count;
    }

    if (event == FW_ISR_SERIAL_ERR) {
        serial->error = FW_EOVERFLOW;
        return 0;
    }
    return FW_ENOTSUP;
}

/* serial 类操作表：open/close 有实现，spi/display 动作不适用。 */
static const fw_dev_ops_t serial_dev_ops = {
    serial_open,
    serial_close,
    serial_read,
    serial_write,
    serial_ioctl,
    serial_isr
};

/*
 * 注册串口设备。
 *
 * 顺序很重要：
 *   1. 先做参数与类型校验；
 *   2. 把基类注册进 core（此时还没有类 ops）；
 *   3. 挂接 serial 类 ops；
 *   4. 保存配置并初始化两个 ring。
 *
 * 如果 ring 初始化失败，设备已经进入注册表，当前实现直接返回错误，
 * 由板级初始化整体失败并 panic；这种“半注册”只在配置错误时出现。
 *
 * 存储区由驱动传入，生命周期必须覆盖整个运行期（通常是静态数组）。
 */
fw_status_t fw_serial_register(fw_serial_t *serial, const char *name,
                               const fw_drv_t *drv,
                               uint8_t *rd_storage, size_t rd_size,
                               uint8_t *wr_storage, size_t wr_size,
                               const fw_serial_config_t *config)
{
    fw_status_t result;
    if (serial == NULL || name == NULL || drv == NULL ||
        rd_storage == NULL || wr_storage == NULL ||
        rd_size == 0U || wr_size == 0U) return FW_EINVAL;
    if (drv->type != FW_DEV_TYPE_SERIAL) return FW_EINVAL;

    result = fw_dev_register(&serial->dev, name, FW_DEV_TYPE_SERIAL, drv);
    if (result != FW_OK) return result;

    serial->dev.ops = &serial_dev_ops;
    serial->config = config != NULL ? *config : default_config;
    serial->error = FW_OK;
    if (fw_ring_init(&serial->rd_fifo, rd_storage, rd_size) != FW_OK ||
        fw_ring_init(&serial->wr_fifo, wr_storage, wr_size) != FW_OK) {
        return FW_EINVAL;
    }
    return FW_OK;
}

/* 查找并做类型校验，返回可安全转换为 fw_serial_t* 的指针。 */
fw_serial_t *fw_serial_find(const char *name)
{
    fw_dev_t *dev = fw_dev_find(name);
    if (dev == NULL || dev->type != FW_DEV_TYPE_SERIAL) return NULL;
    return FW_CONTAINER_OF(dev, fw_serial_t, dev);
}

/* 配置便捷函数：把结构体指针转成 ioctl 参数。 */
int fw_serial_ioctl_config(fw_dev_t *dev, const fw_serial_config_t *config)
{
    if (config == NULL) return FW_EINVAL;
    return fw_dev_ioctl(dev, 0, FW_CTL_SERIAL_SET_CONFIG, (void *)config);
}

/*
 * 等待接收数据。
 *
 * 先在锁内快速检查“是否已有数据/错误”：
 *   - 有则立即返回，避免一次不必要的睡眠；
 *   - 没有则调用驱动的 wait_rx。
 *
 * 这一步很关键：如果先睡眠再检查，ISR 可能刚好在两步之间写入数据，
 * 导致事件丢失、任务一直睡到超时。先检查再等待，配合驱动内部的
 * “等待前再检查一次”，形成双重检查，消除这个竞态窗口。
 */
fw_status_t fw_serial_wait_rx(fw_dev_t *dev, uint32_t timeout_ms)
{
    fw_serial_t *serial;
    const fw_serial_ops_t *ops;
    int level;
    bool ready;

    if (dev == NULL || dev->type != FW_DEV_TYPE_SERIAL) return FW_EINVAL;
    serial = FW_CONTAINER_OF(dev, fw_serial_t, dev);

    level = serial_lock(dev);
    ready = serial->error != FW_OK || serial->rd_fifo.count != 0U;
    serial_unlock(dev, level);
    if (ready) return FW_OK;

    ops = serial_ops(dev);
    if (ops != NULL && ops->wait_rx != NULL) return ops->wait_rx(dev, timeout_ms);
    return timeout_ms == 0U ? FW_EAGAIN : FW_ETIMEOUT;
}
