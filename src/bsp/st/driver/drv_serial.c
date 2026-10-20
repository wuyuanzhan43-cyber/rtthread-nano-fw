/*
 * drv_serial.c - STM32 串口实例表驱动。
 *
 * 层级：src/bsp/st/driver
 * 职责：
 *   1. 按 Kconfig 逐实例初始化 USART、GPIO、HAL、RT 事件和 ring；
 *   2. 实现 serial 类要求的驱动 ops（配置/读写字节/启停 TX/等待 RX/临界区）；
 *   3. 在驱动内部定义 USARTx_IRQHandler，把硬件事件转发给 serial 类；
 *   4. 根据 BOARD_CONSOLE_UART_NUM 选择控制台实例。
 *
 * 上下文：
 *   - init/start/console：任务或启动上下文；
 *   - irq_handler：中断上下文，必须短小、非阻塞；
 *   - lock/unlock：任务与 ISR 共用，使用 PRIMASK 关中断。
 *
 * 依赖：drv_serial.h、board_config.h、fw_config.h、stm32f407_map.h、rthw.h。
 *
 * 中断模型：
 *
 *   RXNE  -> 读 DR -> fw_dev_isr(RD) -> 类写入 rd_fifo -> rt_event_send
 *   TXE   -> fw_dev_isr(WR) -> 类从 wr_fifo 取一字节 -> 写 DR
 *   错误  -> 读 DR 清标志 -> fw_dev_isr(ERR) -> 类记录错误
 *
 * 为什么 TX 用 TXE 中断而不是一次性发送整个 buffer：
 *   这样 write 可以立即返回，不阻塞调用者；TX ring 提供背压，
 *   应用只需要处理短写，不需要等待 UART 硬件。
 */
#include "drv_serial.h"
#include "board_config.h"
#include "fw_config.h"
#include "stm32f407_map.h"
#include <rthw.h>

/* 默认等待时间，保留给未来需要默认超时的路径。 */
#define DRV_SERIAL_TIMEOUT_DEFAULT 20U

/*
 * 毫秒到 RT-Thread tick 的转换。
 *
 * 特殊值：
 *   UINT32_MAX -> RT_WAITING_FOREVER（永久等待）；
 *   0          -> 0（非阻塞）。
 *
 * 其他值用 rt_tick_from_millisecond 转换；如果结果被截断为 0，
 * 至少返回 1 tick，避免“设置了超时却立即返回”的语义错误。
 */
static rt_int32_t ticks_from_ms(uint32_t timeout_ms)
{
    rt_tick_t ticks;
    if (timeout_ms == UINT32_MAX) return RT_WAITING_FOREVER;
    if (timeout_ms == 0U) return 0;
    ticks = rt_tick_from_millisecond((rt_int32_t)timeout_ms);
    return ticks == 0U ? 1 : (rt_int32_t)ticks;
}

/*
 * 配置一个 UART 复用引脚。
 *
 * 参数：
 *   port_index  - 端口编号 0=A..4=E；
 *   pin_number  - 引脚号 0..15；
 *   af          - 复用功能编号（USART 通常为 AF7）；
 *   pull        - 上拉/下拉策略：TX 通常 NOPULL，RX 建议 PULLUP，
 *                 避免悬空时收到噪声起始位。
 *
 * 步骤：解析端口 -> 使能时钟 -> 填 HAL GPIO 初始化结构 -> HAL_GPIO_Init。
 * 所有 USART 引脚都使用推挽复用、最高速度。
 */
static fw_status_t configure_gpio(uint8_t port_index, uint8_t pin_number,
                                  uint8_t af, uint32_t pull)
{
    GPIO_TypeDef *port = stm32f407_gpio(port_index);
    GPIO_InitTypeDef init = {0};
    if (port == NULL || pin_number > 15U) return FW_EINVAL;
    if (stm32f407_gpio_clock_enable(port_index) != FW_OK) return FW_EINVAL;
    init.Pin = (uint16_t)(1U << pin_number);
    init.Mode = GPIO_MODE_AF_PP;
    init.Pull = pull;
    init.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    init.Alternate = af;
    HAL_GPIO_Init(port, &init);
    return FW_OK;
}

/*
 * 应用串口配置到硬件。
 *
 * 先更新 HAL 句柄中的波特率/字长/停止位/校验位，然后调用 HAL_UART_Init。
 *
 * 如果串口已经启动（中断已打开），重新初始化前必须临时关闭 RXNE/ERR
 * 中断，避免 HAL_UART_Init 改写寄存器期间收到半配置状态的数据；
 * 初始化完成后再恢复中断。
 *
 * 注意：这里不重新配置 GPIO，运行期改配置只改变波特率/帧格式，
 * 引脚复用保持不变。
 */
static int serial_configure(fw_dev_t *dev, const fw_serial_config_t *config)
{
    drv_serial_t *serial = FW_CONTAINER_OF(dev, drv_serial_t, base.dev);
    if (serial == NULL || config == NULL || config->baud == 0U) return FW_EINVAL;

    serial->hal.Init.BaudRate = config->baud;
    serial->hal.Init.WordLength = UART_WORDLENGTH_8B;
    serial->hal.Init.StopBits = config->stop_bits == FW_SERIAL_STOP_BITS_2
                                    ? UART_STOPBITS_2 : UART_STOPBITS_1;
    serial->hal.Init.Parity = config->parity == FW_SERIAL_PARITY_EVEN
                                  ? UART_PARITY_EVEN
                                  : (config->parity == FW_SERIAL_PARITY_ODD
                                         ? UART_PARITY_ODD : UART_PARITY_NONE);

    if (serial->started) {
        __HAL_UART_DISABLE_IT(&serial->hal, UART_IT_RXNE);
        __HAL_UART_DISABLE_IT(&serial->hal, UART_IT_ERR);
    }
    if (HAL_UART_Init(&serial->hal) != HAL_OK) return FW_EIO;
    if (serial->started) {
        __HAL_UART_ENABLE_IT(&serial->hal, UART_IT_RXNE);
        __HAL_UART_ENABLE_IT(&serial->hal, UART_IT_ERR);
    }
    return FW_OK;
}

/*
 * 读取一个字节。
 *
 * 直接读 DR：RXNE 已经由中断处理体确认，读 DR 同时清除 RXNE。
 * 返回值 0..255；类型保留 int 是为了与类 ops 的错误码兼容。
 */
static int serial_read_byte(fw_dev_t *dev)
{
    drv_serial_t *serial = FW_CONTAINER_OF(dev, drv_serial_t, base.dev);
    return (int)(serial->instance->DR & 0xFFU);
}

/* 写入一个字节到 DR；调用前 TXE 必须已经置位。 */
static int serial_write_byte(fw_dev_t *dev, uint8_t byte)
{
    drv_serial_t *serial = FW_CONTAINER_OF(dev, drv_serial_t, base.dev);
    serial->instance->DR = byte;
    return 0;
}

/*
 * 启动/继续发送。
 *
 * 只在 tx_active 为 false 时打开 TXE 中断，避免每次 write 都重复使能。
 * 如果串口还没 start（中断未配置），返回 FW_ENOTREADY。
 *
 * TXE 中断打开后，ISR 会不断从 wr_fifo 取字节，直到 ring 空，
 * 然后调用 stop_tx 关闭中断。
 */
static int serial_start_tx(fw_dev_t *dev)
{
    drv_serial_t *serial = FW_CONTAINER_OF(dev, drv_serial_t, base.dev);
    if (!serial->started) return FW_ENOTREADY;
    if (!serial->tx_active) {
        serial->tx_active = true;
        __HAL_UART_ENABLE_IT(&serial->hal, UART_IT_TXE);
    }
    return 0;
}

/* 关闭 TXE 中断并清除发送活跃标志；由类的 WR 事件在 ring 空时调用。 */
static int serial_stop_tx(fw_dev_t *dev)
{
    drv_serial_t *serial = FW_CONTAINER_OF(dev, drv_serial_t, base.dev);
    __HAL_UART_DISABLE_IT(&serial->hal, UART_IT_TXE);
    serial->tx_active = false;
    return 0;
}

/*
 * 等待接收数据。
 *
 * 双重检查：
 *   1. 关中断检查 error/rd_fifo.count；
 *   2. 如果已经就绪，立即返回，避免不必要的睡眠；
 *   3. 否则调用 rt_event_recv 等待事件；
 *   4. 被唤醒后重新循环，再次检查 ring。
 *
 * 为什么需要第一步：
 *   ISR 可能恰好发生在“类检查 ring 为空”和“进入 rt_event_recv”之间。
 *   如果事件在等待前已经发送，rt_event_recv 会立即返回（RT-Thread
 *   事件带清除标志），但先检查一次可以避免无谓的调度。
 *
 * 超时返回 FW_ETIMEOUT；非阻塞且无数据返回 FW_EAGAIN。
 * 事件使用 OR + CLEAR：收到任意一个事件位就清除并返回。
 */
static fw_status_t serial_wait_rx(fw_dev_t *dev, uint32_t timeout_ms)
{
    drv_serial_t *serial = FW_CONTAINER_OF(dev, drv_serial_t, base.dev);
    for (;;) {
        rt_base_t level = rt_hw_interrupt_disable();
        bool ready = serial->base.error != FW_OK ||
                     serial->base.rd_fifo.count != 0U;
        rt_hw_interrupt_enable(level);
        if (ready) return FW_OK;
        if (timeout_ms == 0U) return FW_EAGAIN;
        {
            rt_uint32_t received = 0U;
            if (rt_event_recv(&serial->rx_event, 1U,
                              RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR,
                              ticks_from_ms(timeout_ms), &received) != RT_EOK)
                return FW_ETIMEOUT;
        }
    }
}

/*
 * 进入临界区。
 *
 * 使用 PRIMASK 关闭所有可屏蔽中断，返回之前的状态。
 * 这样任务侧保护 ring 时，USART ISR 不会同时修改 head/tail/count。
 *
 * 临界区必须非常短：当前只在 ring 读写和状态检查时使用，
 * 不会在锁内调用 HAL_UART_Init 等耗时函数。
 */
static int serial_lock(fw_dev_t *dev)
{
    (void)dev;
    return (int)rt_hw_interrupt_disable();
}

/* 恢复 lock 之前的中断状态。 */
static void serial_unlock(fw_dev_t *dev, int level)
{
    (void)dev;
    rt_hw_interrupt_enable((rt_base_t)level);
}

/* serial 驱动 ops：与 fw_serial_ops_t 的字段顺序一一对应。 */
static const fw_serial_ops_t drv_serial_ops = {
    serial_configure,
    serial_read_byte,
    serial_write_byte,
    serial_start_tx,
    serial_stop_tx,
    serial_wait_rx,
    serial_lock,
    serial_unlock
};

/*
 * 初始化一个串口实例。
 *
 * 参数：
 *   serial   - 静态实例；
 *   name     - "serial1" 等设备名；
 *   instance - USART 编号 1..3；
 *   tx/rx    - 引脚端口/编号；
 *   af       - 复用功能号。
 *
 * 步骤：
 *   1. 通过 map 模块解析 USART 指针和 IRQ 号；
 *   2. 配置 TX/RX GPIO；
 *   3. 使能 USART 时钟，填写 HAL 初始化参数并 HAL_UART_Init；
 *   4. 初始化 RT 事件（用于 wait_rx）；
 *   5. 填写驱动描述，注册 serial 类设备（传入静态 ring 存储）。
 *
 * 注意：这里不打开 NVIC 中断，也不使能 RXNE/TXE；
 * 这些动作在 serial_start_instance 中完成，保证设备已注册。
 */
static fw_status_t serial_init_instance(drv_serial_t *serial, const char *name,
                                        uint8_t instance,
                                        uint8_t tx_port, uint8_t tx_pin,
                                        uint8_t rx_port, uint8_t rx_pin,
                                        uint8_t af)
{
    fw_serial_config_t config;
    HAL_StatusTypeDef status;
    fw_status_t result;

    serial->instance = stm32f407_usart(instance);
    serial->irq = stm32f407_usart_irq(instance);
    if (serial->instance == NULL || (int)serial->irq < 0) return FW_EINVAL;

    /* TX 浮空，RX 上拉：减少悬空引脚在空闲时收到噪声。 */
    result = configure_gpio(tx_port, tx_pin, af, GPIO_NOPULL);
    if (result != FW_OK) return result;
    result = configure_gpio(rx_port, rx_pin, af, GPIO_PULLUP);
    if (result != FW_OK) return result;

    stm32f407_usart_clock_enable(instance);
    serial->hal.Instance = serial->instance;
    serial->hal.Init.BaudRate = BOARD_UART_BAUD;
    serial->hal.Init.WordLength = UART_WORDLENGTH_8B;
    serial->hal.Init.StopBits = UART_STOPBITS_1;
    serial->hal.Init.Parity = UART_PARITY_NONE;
    serial->hal.Init.Mode = UART_MODE_TX_RX;
    serial->hal.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    serial->hal.Init.OverSampling = UART_OVERSAMPLING_16;
    status = HAL_UART_Init(&serial->hal);
    if (status != HAL_OK) return FW_EIO;

    /* 事件名使用设备名，便于调试器/日志识别是哪个串口的事件。 */
    if (rt_event_init(&serial->rx_event, name, RT_IPC_FLAG_FIFO) != RT_EOK)
        return FW_EIO;

    config.baud = BOARD_UART_BAUD;
    config.data_bits = FW_SERIAL_DATA_BITS_8;
    config.stop_bits = FW_SERIAL_STOP_BITS_1;
    config.parity = FW_SERIAL_PARITY_NONE;

    serial->drv.type = FW_DEV_TYPE_SERIAL;
    serial->drv.ops = &drv_serial_ops;
    serial->drv.data = serial;
    serial->started = false;
    serial->tx_active = false;

    /* ring 存储区属于驱动实例，类只保存指针和控制块。 */
    return fw_serial_register(&serial->base, name, &serial->drv,
                              serial->rx_storage, sizeof(serial->rx_storage),
                              serial->tx_storage, sizeof(serial->tx_storage),
                              &config);
}

/*
 * 启动一个串口实例。
 *
 * 设置 NVIC 优先级并使能中断；初始只打开 RXNE 和 ERR，
 * TXE 在第一次 write 时由 serial_start_tx 按需打开。
 *
 * started=true 之后，serial_configure 才会在重配置时临时关中断。
 */
static void serial_start_instance(drv_serial_t *serial)
{
    if (serial == NULL || serial->started) return;
    HAL_NVIC_SetPriority(serial->irq, BOARD_UART_IRQ_PRIORITY, 0U);
    HAL_NVIC_EnableIRQ(serial->irq);
    __HAL_UART_DISABLE_IT(&serial->hal, UART_IT_TXE);
    __HAL_UART_ENABLE_IT(&serial->hal, UART_IT_RXNE);
    __HAL_UART_ENABLE_IT(&serial->hal, UART_IT_ERR);
    serial->started = true;
}

/*
 * 静态实例定义。
 *
 * 只有 Kconfig 打开对应 UART 时才分配结构体和 ring 存储；
 * 关闭的实例不占用 RAM/Flash，也不会生成中断函数。
 * 每个实例的 ring 大小由 FW_SERIAL_RX_BUFSZ / FW_SERIAL_TX_BUFSZ 决定。
 */
#if defined(FW_USING_UART1) && FW_USING_UART1
static drv_serial_t serial1;
#endif
#if defined(FW_USING_UART2) && FW_USING_UART2
static drv_serial_t serial2;
#endif
#if defined(FW_USING_UART3) && FW_USING_UART3
static drv_serial_t serial3;
#endif

/*
 * 注册所有已启用的串口。
 *
 * 每个实例的引脚和 AF 来自 board_config.h（由 Kconfig 生成），
 * 因此新增 UART 实例只需要改 Kconfig，不需要改这个函数。
 * 任何一个实例失败都会立即返回，由 board 初始化统一 panic。
 */
fw_status_t drv_serial_register_all(void)
{
    fw_status_t result = FW_OK;

#if defined(FW_USING_UART1) && FW_USING_UART1
    result = serial_init_instance(&serial1, "serial1", 1,
                                  BOARD_UART1_TX_PORT, BOARD_UART1_TX_PIN,
                                  BOARD_UART1_RX_PORT, BOARD_UART1_RX_PIN,
                                  BOARD_UART1_AF);
    if (result != FW_OK) return result;
#endif
#if defined(FW_USING_UART2) && FW_USING_UART2
    result = serial_init_instance(&serial2, "serial2", 2,
                                  BOARD_UART2_TX_PORT, BOARD_UART2_TX_PIN,
                                  BOARD_UART2_RX_PORT, BOARD_UART2_RX_PIN,
                                  BOARD_UART2_AF);
    if (result != FW_OK) return result;
#endif
#if defined(FW_USING_UART3) && FW_USING_UART3
    result = serial_init_instance(&serial3, "serial3", 3,
                                  BOARD_UART3_TX_PORT, BOARD_UART3_TX_PIN,
                                  BOARD_UART3_RX_PORT, BOARD_UART3_RX_PIN,
                                  BOARD_UART3_AF);
    if (result != FW_OK) return result;
#endif
    return result;
}

/* 打开所有已启用串口的中断；重复调用安全。 */
fw_status_t drv_serial_start_all(void)
{
#if defined(FW_USING_UART1) && FW_USING_UART1
    serial_start_instance(&serial1);
#endif
#if defined(FW_USING_UART2) && FW_USING_UART2
    serial_start_instance(&serial2);
#endif
#if defined(FW_USING_UART3) && FW_USING_UART3
    serial_start_instance(&serial3);
#endif
    return FW_OK;
}

/*
 * 返回控制台串口实例。
 *
 * 条件编译同时检查：
 *   - 该 UART 已被 Kconfig 启用；
 *   - BOARD_CONSOLE_UART_NUM 指向它。
 * 如果配置不一致（例如控制台选了 UART2 但 UART2 未启用），
 * genconfig 会在生成阶段报错；这里再返回 NULL 作为运行期兜底。
 */
drv_serial_t *drv_serial_console(void)
{
#if defined(FW_USING_UART1) && FW_USING_UART1 && (BOARD_CONSOLE_UART_NUM == 1)
    return &serial1;
#elif defined(FW_USING_UART2) && FW_USING_UART2 && (BOARD_CONSOLE_UART_NUM == 2)
    return &serial2;
#elif defined(FW_USING_UART3) && FW_USING_UART3 && (BOARD_CONSOLE_UART_NUM == 3)
    return &serial3;
#else
    return NULL;
#endif
}

/* 控制台设备基类指针，供应用层 open/read/write。 */
fw_dev_t *drv_serial_console_dev(void)
{
    drv_serial_t *serial = drv_serial_console();
    return serial != NULL ? &serial->base.dev : NULL;
}

/*
 * 统一 USART 中断处理体。
 *
 * 进入时已由 USARTx_IRQHandler 调用 rt_interrupt_enter()。
 *
 * 流程：
 *   1. 读一次 SR，避免多次读寄存器时状态变化；
 *   2. RXNE 置位：调用类的 RD 事件读取 DR 并写入 ring；
 *      同时置 wake=true，稍后发送 rt_event；
 *   3. 错误标志（ORE/FE/NE/PE）：读 DR 清除标志，
 *      调用类 ERR 事件记录错误，并唤醒等待任务；
 *   4. TXE 置位：调用类 WR 事件，从 ring 取下一字节或关闭 TXE；
 *   5. 如果有新数据或错误，发送 rx_event 唤醒等待中的 read/wait_rx。
 *
 * 注意：
 *   - 错误标志和 RXNE 可能同时置位，因此错误检查独立于 RXNE 分支；
 *   - 读 DR 是清除 ORE 等标志的必要步骤；
 *   - rt_event_send 在中断中调用是安全的，它会触发调度挂起，
 *     真正切换发生在中断退出时。
 */
void drv_serial_irq_handler(drv_serial_t *serial)
{
    uint32_t sr;
    bool have_byte;
    bool wake = false;

    if (serial == NULL || !serial->started) return;
    sr = serial->instance->SR;

    have_byte = (sr & USART_SR_RXNE) != 0U;
    if (have_byte) {
        (void)fw_dev_isr(&serial->base.dev, FW_ISR_SERIAL_RD, NULL);
        wake = true;
    } else if ((sr & (USART_SR_ORE | USART_SR_FE | USART_SR_NE | USART_SR_PE)) != 0U) {
        /* 错误标志需要读 DR 才能清除；数据本身被丢弃。 */
        (void)serial->instance->DR;
    }

    if ((sr & (USART_SR_ORE | USART_SR_FE | USART_SR_NE | USART_SR_PE)) != 0U) {
        (void)fw_dev_isr(&serial->base.dev, FW_ISR_SERIAL_ERR, NULL);
        wake = true;
    }

    if ((sr & USART_SR_TXE) != 0U) {
        /* 类会从 wr_fifo 取字节；ring 空时关闭 TXE。 */
        (void)fw_dev_isr(&serial->base.dev, FW_ISR_SERIAL_WR, NULL);
    }

    /* 有新数据/错误时唤醒等待任务；TXE 事件不需要唤醒。 */
    if (wake) (void)rt_event_send(&serial->rx_event, 1U);
}

/*
 * USART1 中断入口。
 *
 * rt_interrupt_enter/leave 让 RT-Thread 知道当前在中断上下文，
 * 从而正确处理调度挂起和中断嵌套计数。
 * 只有 Kconfig 启用对应 UART 时才生成函数，避免与启动文件中的
 * 默认弱符号冲突。
 */
#if defined(FW_USING_UART1) && FW_USING_UART1
void USART1_IRQHandler(void)
{
    rt_interrupt_enter();
    drv_serial_irq_handler(&serial1);
    rt_interrupt_leave();
}
#endif
#if defined(FW_USING_UART2) && FW_USING_UART2
void USART2_IRQHandler(void)
{
    rt_interrupt_enter();
    drv_serial_irq_handler(&serial2);
    rt_interrupt_leave();
}
#endif
#if defined(FW_USING_UART3) && FW_USING_UART3
void USART3_IRQHandler(void)
{
    rt_interrupt_enter();
    drv_serial_irq_handler(&serial3);
    rt_interrupt_leave();
}
#endif
