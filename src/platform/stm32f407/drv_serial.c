/*
 * drv_serial.c - STM32 串口实例表驱动。
 *
 * 层级：src/platform/stm32f407
 * 职责：
 *   1. 按 Kconfig 逐实例初始化 USART、GPIO、HAL、RT 事件和 ring；
 *   2. 实现 serial 类要求的驱动 ops（配置/读写字节/启停 TX/等待 RX/临界区）；
 *   3. 在驱动内部定义 USARTx_IRQHandler，把硬件事件转发给 serial 类；
 *   4. 从板级描述符接收引脚/AF/波特率/IRQ 优先级。
 *
 * 上下文：
 *   - init/start：任务或启动上下文；
 *   - irq_handler：中断上下文，必须短小、非阻塞；
 *   - lock/unlock：任务与 ISR 共用，使用 PRIMASK 关中断。
 *
 * 依赖：drv_serial.h、fw_config.h、stm32f407_map.h、rthw.h。
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
#include "fw_config.h"
#include "stm32f407_map.h"
#include <rthw.h>

/* 默认等待时间，保留给未来需要默认超时的路径。 */
#define DRV_SERIAL_TIMEOUT_DEFAULT 20U

/*
 * 前向声明：configure 在文件靠前的位置就要停止/重启 RX/TX，
 * 而这些函数的完整实现放在 read/write 辅助函数之后。
 * 显式声明可以避免 C 的隐式 int 声明和 -Werror 冲突。
 */
static fw_status_t serial_hw_start_rx(drv_serial_t *serial);
static void serial_hw_stop_rx(drv_serial_t *serial);
static int serial_start_tx(fw_dev_t *dev);
static int serial_stop_tx(fw_dev_t *dev);

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

    /*
     * 重配前先停掉所有在途 DMA/中断。
     *
     * HAL_UART_Init 会重写 CR1/CR2/CR3，如果 DMA 还在搬运，旧配置下的
     * 数据会和新配置混在一起；TX 在途数据在改波特率后也没有意义，
     * 因此这里明确丢弃。重配完成后只恢复 RX，TX 等下一次 write 再启动。
     */
    if (serial->started) {
        serial_hw_stop_rx(serial);
        (void)serial_stop_tx(&serial->base.dev);
        __HAL_UART_DISABLE_IT(&serial->hal, UART_IT_ERR);
        __HAL_UART_DISABLE_IT(&serial->hal, UART_IT_TXE);
    }
    if (HAL_UART_Init(&serial->hal) != HAL_OK) return FW_EIO;
    if (serial->started) {
        if (serial_hw_start_rx(serial) != FW_OK) return FW_EIO;
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
 * 启动 RX。
 *
 * 两条路径互斥：
 *   - DMA：RXNE 必须关闭，否则 USART 中断也会读 DR，和 DMA 抢数据；
 *     circular DMA 的缓冲区是 2*CHUNK，半传输/全传输各完成一半；
 *   - IT：打开 RXNE，沿用原来的逐字节 ISR。
 *
 * 无论哪条路径都保留 ERR 中断：ORE/FE/NE/PE 需要驱动读取 SR/DR 清除；
 * DMA 传输错误（TE）由 DMA IRQ 的 HAL_UART_ErrorCallback 负责恢复。
 */
static fw_status_t serial_hw_start_rx(drv_serial_t *serial)
{
    if (serial->dma_rx_enabled) {
        __HAL_UART_DISABLE_IT(&serial->hal, UART_IT_RXNE);
        serial->dma_rx_active = false;
        if (!drv_dma_ptr_ok(serial->rx_dma_buf, sizeof(serial->rx_dma_buf)))
            return FW_EINVAL;
        if (HAL_UART_Receive_DMA(&serial->hal, serial->rx_dma_buf,
                                 (uint16_t)sizeof(serial->rx_dma_buf)) != HAL_OK)
            return FW_EIO;
        serial->dma_rx_active = true;
        return FW_OK;
    }
    __HAL_UART_ENABLE_IT(&serial->hal, UART_IT_RXNE);
    return FW_OK;
}

/*
 * 停止 RX。
 *
 * HAL_UART_DMAStop 同时停止 RX/TX DMA；配置或错误恢复时使用它是安全的，
 * 因为这两个场景都允许丢弃在途数据。dma_rx_active 必须同步复位，
 * 否则后续状态机会误以为 DMA 还在搬运。
 */
static void serial_hw_stop_rx(drv_serial_t *serial)
{
    if (serial->dma_rx_enabled) {
        /*
         * HAL_UART_DMAStop 同时停止 RX/TX；先清两个 active 和 staging 长度，
         * 这样 abort 如果触发完成回调，不会被误认为正常传输完成。
         */
        serial->dma_rx_active = false;
        serial->dma_tx_active = false;
        serial->tx_stage_len = 0U;
        (void)HAL_UART_DMAStop(&serial->hal);
    } else {
        __HAL_UART_DISABLE_IT(&serial->hal, UART_IT_RXNE);
    }
}

/*
 * 启动/继续发送。
 *
 * IT 路径：
 *   打开 TXE，ISR 每次从 wr_fifo 取一个字节写 DR；ring 空时 stop_tx。
 *
 * DMA 路径：
 *   - 用 fw_serial_tx_peek 把 ring 中最早的一块复制到 staging；
 *     不能直接 read，否则 DMA 还在读 staging 时 ring 可能回绕覆盖数据；
 *   - 启动 HAL_UART_Transmit_DMA，完成回调里再 consume；
 *   - tx_stage_len 记录本块长度，供完成回调精确消费；
 *   - dma_tx_active 防止重复启动同一块。
 *
 * 该函数既可能由任务 write 调用，也可能由 DMA TX 完成中断调用
 * （继续发送下一块）；两者都在串口类的上下文语义内。
 */
static int serial_start_tx(fw_dev_t *dev)
{
    drv_serial_t *serial = FW_CONTAINER_OF(dev, drv_serial_t, base.dev);
    if (!serial->started) return FW_ENOTREADY;

    if (serial->dma_tx_enabled) {
        size_t count;
        if (serial->dma_tx_active) return 0;
        if (!drv_dma_ptr_ok(serial->tx_dma_buf, sizeof(serial->tx_dma_buf)))
            return FW_EINVAL;
        count = fw_serial_tx_peek(&serial->base, serial->tx_dma_buf,
                                  sizeof(serial->tx_dma_buf));
        if (count == 0U) return 0;
        serial->tx_stage_len = (uint16_t)count;
        serial->dma_tx_active = true;
        if (HAL_UART_Transmit_DMA(&serial->hal, serial->tx_dma_buf,
                                  (uint16_t)count) != HAL_OK) {
            serial->dma_tx_active = false;
            serial->tx_stage_len = 0U;
            return FW_EIO;
        }
        return 0;
    }

    if (!serial->tx_active) {
        serial->tx_active = true;
        __HAL_UART_ENABLE_IT(&serial->hal, UART_IT_TXE);
    }
    return 0;
}

/*
 * 停止发送。
 *
 * DMA 路径必须 abort，否则清空/重配后硬件还会继续读 staging；
 * 已经进入 staging 的字节可能已经发出，这是清发送缓冲的已知语义：
 * 调用者要求“停止并丢弃”，不保证对端没有收到已经发出的前缀。
 */
static int serial_stop_tx(fw_dev_t *dev)
{
    drv_serial_t *serial = FW_CONTAINER_OF(dev, drv_serial_t, base.dev);
    if (serial->dma_tx_enabled && serial->dma_tx_active) {
        /* 先清状态，避免 abort 内部回调把 staging 误当作已发送而消费。 */
        serial->dma_tx_active = false;
        serial->tx_stage_len = 0U;
        (void)HAL_UART_AbortTransmit(&serial->hal);
    }
    __HAL_UART_DISABLE_IT(&serial->hal, UART_IT_TXE);
    serial->tx_active = false;
    return 0;
}

/*
 * 类层 CLR_WR_BUF 的驱动侧钩子。
 *
 * RX 清理由类层在锁内直接清 ring 完成，DMA 可以继续运行；
 * TX 必须先把在途 DMA 停掉，再让类层清 ring。
 */
static int serial_flush_ops(fw_dev_t *dev, bool rx)
{
    if (!rx) return serial_stop_tx(dev);
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
    serial_unlock,
    serial_flush_ops
};

/*
 * 初始化一个 DMA handle。
 *
 * 只做“DMA 句柄 + 路由”的通用配置；controller 时钟和 NVIC 由
 * drv_dma_claim 统一管理。调用顺序很重要：
 *   1. 设置 Instance（drv_dma_claim 会校验它和 route 一致）；
 *   2. claim 打开 DMA 时钟并记录 owner；
 *   3. HAL_DMA_Init 写寄存器；
 *   4. __HAL_LINKDMA 把句柄挂到 UART。
 *
 * RX 使用 circular 模式，配合半传输/全传输回调形成双缓冲；
 * TX 使用 normal 模式，一块完成后由回调再启动下一块。
 */
static fw_status_t serial_dma_setup_one(drv_serial_t *serial,
                                        DMA_HandleTypeDef *hdma,
                                        const drv_dma_route_t *route,
                                        bool rx, const char *owner)
{
    uint32_t direction;
    uint32_t mode;
    uint32_t channel;

    if (hdma == NULL || route == NULL) return FW_EINVAL;
    hdma->Instance = stm32f407_dma_stream(route->controller, route->stream);
    if (hdma->Instance == NULL) return FW_EINVAL;
    channel = drv_dma_channel_value(route->channel);
    if (channel == 0xFFFFFFFFU) return FW_EINVAL;

    /*
     * F4 的 DMA 请求由 channel 决定，方向/循环模式由 stream 配置。
     * FIFO 关闭、字节对齐：UART 数据是 8 位，不需要打包。
     */
    direction = rx ? DMA_PERIPH_TO_MEMORY : DMA_MEMORY_TO_PERIPH;
    mode = rx ? DMA_CIRCULAR : DMA_NORMAL;
    hdma->Init.Channel = channel;
    hdma->Init.Direction = direction;
    hdma->Init.PeriphInc = DMA_PINC_DISABLE;
    hdma->Init.MemInc = DMA_MINC_ENABLE;
    hdma->Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
    hdma->Init.MemDataAlignment = DMA_MDATAALIGN_BYTE;
    hdma->Init.Mode = mode;
    hdma->Init.Priority = DMA_PRIORITY_HIGH;
    hdma->Init.FIFOMode = DMA_FIFOMODE_DISABLE;
    hdma->Init.FIFOThreshold = DMA_FIFO_THRESHOLD_HALFFULL;
    hdma->Init.MemBurst = DMA_MBURST_SINGLE;
    hdma->Init.PeriphBurst = DMA_PBURST_SINGLE;

    if (drv_dma_claim(hdma, route, owner) != FW_OK) return FW_EBUSY;
    if (HAL_DMA_Init(hdma) != HAL_OK) {
        drv_dma_release(hdma);
        return FW_EIO;
    }
    if (rx) __HAL_LINKDMA(&serial->hal, hdmarx, serial->hdma_rx);
    else __HAL_LINKDMA(&serial->hal, hdmatx, serial->hdma_tx);
    return FW_OK;
}

/* DMA owner 诊断名；用实例号索引，避免在中断路径里格式化字符串。 */
static const char *serial_dma_owner(uint8_t instance, bool rx)
{
    static const char *const rx_names[3] = {
        "serial1.rx", "serial2.rx", "serial3.rx"
    };
    static const char *const tx_names[3] = {
        "serial1.tx", "serial2.tx", "serial3.tx"
    };
    if (instance >= 1U && instance <= 3U)
        return rx ? rx_names[instance - 1U] : tx_names[instance - 1U];
    return rx ? "uart.rx" : "uart.tx";
}

/*
 * 只在 Kconfig 打开对应 DMA 开关时初始化 RX/TX 句柄。
 *
 * 未启用时把路由清成 DRV_DMA_NONE，后续 serial_start_tx/serial_hw_start_rx
 * 会走原来的 IT 路径；这样同一个驱动结构体可以同时服务两种配置，
 * 不需要在 C 代码里按实例写 #if。
 */
static fw_status_t serial_dma_setup(drv_serial_t *serial,
                                    const drv_serial_hw_t *hw)
{
    serial->dma_rx_route = hw->dma_rx;
    serial->dma_tx_route = hw->dma_tx;
    serial->dma_rx_enabled = hw->dma_rx_enabled;
    serial->dma_tx_enabled = hw->dma_tx_enabled;

    if (!serial->dma_rx_enabled) {
        serial->dma_rx_route.controller = DRV_DMA_NONE;
        serial->dma_rx_route.stream = DRV_DMA_NONE;
        serial->dma_rx_route.channel = DRV_DMA_NONE;
    } else {
        fw_status_t result = serial_dma_setup_one(
            serial, &serial->hdma_rx, &serial->dma_rx_route, true,
            serial_dma_owner(hw->instance, true));
        if (result != FW_OK) return result;
    }

    if (!serial->dma_tx_enabled) {
        serial->dma_tx_route.controller = DRV_DMA_NONE;
        serial->dma_tx_route.stream = DRV_DMA_NONE;
        serial->dma_tx_route.channel = DRV_DMA_NONE;
    } else {
        fw_status_t result = serial_dma_setup_one(
            serial, &serial->hdma_tx, &serial->dma_tx_route, false,
            serial_dma_owner(hw->instance, false));
        if (result != FW_OK) return result;
    }
    return FW_OK;
}

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
 *   5. 按需初始化 DMA 句柄并 claim stream；
 *   6. 填写驱动描述，注册 serial 类设备（传入静态 ring 存储）。
 *
 * 注意：这里不打开 NVIC 中断，也不启动 RX/TX DMA；
 * 这些动作在 serial_start_instance / serial_start_tx 中完成，
 * 保证设备已注册、ring 已就绪。
 */
static fw_status_t serial_init_instance(drv_serial_t *serial, const char *name,
                                        const drv_serial_hw_t *hw)
{
    fw_serial_config_t config;
    HAL_StatusTypeDef status;
    fw_status_t result;
    uint8_t instance;

    if (serial == NULL || name == NULL || hw == NULL) return FW_EINVAL;
    instance = hw->instance;
    serial->instance = stm32f407_usart(instance);
    serial->irq = stm32f407_usart_irq(instance);
    if (serial->instance == NULL || (int)serial->irq < 0) return FW_EINVAL;
    serial->irq_priority = hw->irq_priority;

    /* TX 浮空，RX 上拉：减少悬空引脚在空闲时收到噪声。 */
    result = configure_gpio(hw->tx_port, hw->tx_pin, hw->tx_af, GPIO_NOPULL);
    if (result != FW_OK) return result;
    result = configure_gpio(hw->rx_port, hw->rx_pin, hw->rx_af, GPIO_PULLUP);
    if (result != FW_OK) return result;

    stm32f407_usart_clock_enable(instance);
    serial->hal.Instance = serial->instance;
    serial->hal.Init.BaudRate = hw->baud;
    serial->hal.Init.WordLength = UART_WORDLENGTH_8B;
    serial->hal.Init.StopBits = UART_STOPBITS_1;
    serial->hal.Init.Parity = UART_PARITY_NONE;
    serial->hal.Init.Mode = UART_MODE_TX_RX;
    serial->hal.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    serial->hal.Init.OverSampling = UART_OVERSAMPLING_16;
    status = HAL_UART_Init(&serial->hal);
    if (status != HAL_OK) return FW_EIO;

    /* DMA 句柄必须在 HAL_UART_Init 之后绑定，避免 HAL 初始化覆盖关联。 */
    result = serial_dma_setup(serial, hw);
    if (result != FW_OK) return result;

    /* 事件名使用设备名，便于调试器/日志识别是哪个串口的事件。 */
    if (rt_event_init(&serial->rx_event, name, RT_IPC_FLAG_FIFO) != RT_EOK)
        return FW_EIO;

    config.baud = hw->baud;
    config.data_bits = FW_SERIAL_DATA_BITS_8;
    config.stop_bits = FW_SERIAL_STOP_BITS_1;
    config.parity = FW_SERIAL_PARITY_NONE;

    serial->drv.type = FW_DEV_TYPE_SERIAL;
    serial->drv.ops = &drv_serial_ops;
    serial->drv.data = serial;
    serial->started = false;
    serial->tx_active = false;
    serial->dma_rx_active = false;
    serial->dma_tx_active = false;
    serial->tx_stage_len = 0U;

    /* ring 存储区属于驱动实例，类只保存指针和控制块。 */
    return fw_serial_register(&serial->base, name, &serial->drv,
                              serial->rx_storage, sizeof(serial->rx_storage),
                              serial->tx_storage, sizeof(serial->tx_storage),
                              &config);
}

/*
 * 启动一个串口实例。
 *
 * 设置 NVIC 优先级并使能 USART 中断，然后：
 *   - DMA RX：HAL_UART_Receive_DMA 进入 circular 模式；
 *   - IT RX：打开 RXNE。
 * 两种路径都打开 ERR；TXE 在第一次 write 时按需打开。
 *
 * started=true 之后，serial_configure 才知道需要保存/恢复这些硬件状态。
 */
static fw_status_t serial_start_instance(drv_serial_t *serial)
{
    if (serial == NULL || serial->started) return FW_OK;
    HAL_NVIC_SetPriority(serial->irq, serial->irq_priority, 0U);
    HAL_NVIC_EnableIRQ(serial->irq);
    __HAL_UART_DISABLE_IT(&serial->hal, UART_IT_TXE);
    if (serial_hw_start_rx(serial) != FW_OK) {
        HAL_NVIC_DisableIRQ(serial->irq);
        return FW_EIO;
    }
    __HAL_UART_ENABLE_IT(&serial->hal, UART_IT_ERR);
    serial->started = true;
    return FW_OK;
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

/* 实例编号 -> 静态对象；未启用返回 NULL。 */
static drv_serial_t *serial_slot(uint8_t instance)
{
    switch (instance) {
#if defined(FW_USING_UART1) && FW_USING_UART1
    case 1: return &serial1;
#endif
#if defined(FW_USING_UART2) && FW_USING_UART2
    case 2: return &serial2;
#endif
#if defined(FW_USING_UART3) && FW_USING_UART3
    case 3: return &serial3;
#endif
    default: return NULL;
    }
}

/* 实例编号 -> 设备名；只用于已启用实例。 */
static const char *serial_name(uint8_t instance)
{
    switch (instance) {
    case 1: return "serial1";
    case 2: return "serial2";
    case 3: return "serial3";
    default: return NULL;
    }
}

/*
 * HAL 回调只有全局 weak 符号，没有实例参数；必须通过 huart 指针反查
 * 驱动实例，否则多串口同时启用时回调和错误会串到别的串口。
 */
static drv_serial_t *serial_from_huart(UART_HandleTypeDef *huart)
{
#if defined(FW_USING_UART1) && FW_USING_UART1
    if (huart == &serial1.hal) return &serial1;
#endif
#if defined(FW_USING_UART2) && FW_USING_UART2
    if (huart == &serial2.hal) return &serial2;
#endif
#if defined(FW_USING_UART3) && FW_USING_UART3
    if (huart == &serial3.hal) return &serial3;
#endif
    return NULL;
}

/*
 * circular RX DMA：前半区完成。
 *
 * 半传输中断只表示“前半块已经写满”，DMA 仍在向后半区写入；
 * 此时把前半块交给类层是安全的，后半区不会被这次拷贝触碰。
 * 处理完立即 rt_event_send，让等待 wait_rx 的任务醒来。
 */
void HAL_UART_RxHalfCpltCallback(UART_HandleTypeDef *huart)
{
    drv_serial_t *serial = serial_from_huart(huart);
    if (serial == NULL || !serial->dma_rx_enabled || !serial->dma_rx_active)
        return;
    (void)fw_serial_rx_push(&serial->base, serial->rx_dma_buf,
                            FW_SERIAL_DMA_CHUNK);
    (void)rt_event_send(&serial->rx_event, 1U);
}

/*
 * circular RX DMA：后半区完成。
 *
 * 此时 DMA 已经回绕到前半区；后半块已经完整，可以安全复制。
 * 不在这里重新启动 DMA：circular 模式由硬件自动续传。
 */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    drv_serial_t *serial = serial_from_huart(huart);
    if (serial == NULL || !serial->dma_rx_enabled || !serial->dma_rx_active)
        return;
    (void)fw_serial_rx_push(&serial->base,
                            serial->rx_dma_buf + FW_SERIAL_DMA_CHUNK,
                            FW_SERIAL_DMA_CHUNK);
    (void)rt_event_send(&serial->rx_event, 1U);
}

/*
 * TX DMA 完成。
 *
 * 先消费 ring 中对应的 tx_stage_len 字节，再检查是否还有数据：
 *   - 有：立即 peek 下一块并启动新的 DMA；
 *   - 无：清空 active 标志，等待下一次 write。
 *
 * 注意：消费必须发生在 DMA 完成之后；peek/staging 的稳定性依赖
 * “tail 在发送期间不移动”这一约定。
 */
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
    drv_serial_t *serial = serial_from_huart(huart);
    uint16_t consumed;

    if (serial == NULL || !serial->dma_tx_enabled || !serial->dma_tx_active)
        return;
    consumed = serial->tx_stage_len;
    serial->tx_stage_len = 0U;
    serial->dma_tx_active = false;
    if (consumed != 0U) (void)fw_serial_tx_consume(&serial->base, consumed);
    if (serial->base.wr_fifo.count != 0U)
        (void)serial_start_tx(&serial->base.dev);
}

/*
 * UART 错误回调。
 *
 * ORE 映射为 FW_EOVERFLOW，其余 FE/NE/PE 映射为 FW_EIO。错误会唤醒
 * wait_rx，任务下一次 read 会拿到错误并清空 RX ring（类层既有语义）。
 *
 * DMA 模式下必须停止并重新 arm RX DMA，否则 HAL 可能停在 BUSY 状态，
 * 后续数据不再进入 ring。TX 在途数据在错误时直接丢弃，避免把可能
 * 已损坏的前缀继续发送。
 */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    drv_serial_t *serial = serial_from_huart(huart);
    fw_status_t error;

    if (serial == NULL) return;
    error = (huart->ErrorCode & HAL_UART_ERROR_ORE) != 0U
                ? FW_EOVERFLOW : FW_EIO;
    serial->base.error = error;

    if (serial->dma_rx_enabled || serial->dma_tx_enabled) {
        serial->dma_rx_active = false;
        serial->dma_tx_active = false;
        serial->tx_stage_len = 0U;
        serial->tx_active = false;
        (void)HAL_UART_DMAStop(&serial->hal);
        if (serial->started && serial->dma_rx_enabled) {
            (void)serial_hw_start_rx(serial);
            __HAL_UART_ENABLE_IT(&serial->hal, UART_IT_ERR);
        }
    }
    (void)rt_event_send(&serial->rx_event, 1U);
}

/*
 * 按板级描述符注册串口。
 *
 * 描述符表由 board_pins.c 提供；驱动只负责实例对象和 HAL 初始化。
 */
fw_status_t drv_serial_register_all(const drv_serial_hw_t *hw, size_t count)
{
    fw_status_t result;
    size_t i;

    if (count == 0U) return FW_OK;
    if (hw == NULL) return FW_EINVAL;

    /* DMA owner 表在第一次注册前清零；所有实例共享同一组 stream。 */
    drv_dma_init();
    for (i = 0U; i < count; ++i) {
        drv_serial_t *slot = serial_slot(hw[i].instance);
        const char *name = serial_name(hw[i].instance);

        if (slot == NULL || name == NULL) return FW_ENOTSUP;
        result = serial_init_instance(slot, name, &hw[i]);
        if (result != FW_OK) return result;
    }
    return FW_OK;
}

/* 打开所有已启用串口的 RX 路径；重复调用安全。 */
fw_status_t drv_serial_start_all(void)
{
    fw_status_t result;
#if defined(FW_USING_UART1) && FW_USING_UART1
    result = serial_start_instance(&serial1);
    if (result != FW_OK) return result;
#endif
#if defined(FW_USING_UART2) && FW_USING_UART2
    result = serial_start_instance(&serial2);
    if (result != FW_OK) return result;
#endif
#if defined(FW_USING_UART3) && FW_USING_UART3
    result = serial_start_instance(&serial3);
    if (result != FW_OK) return result;
#endif
    return FW_OK;
}

drv_serial_t *drv_serial_instance(uint8_t instance)
{
    return serial_slot(instance);
}

fw_dev_t *drv_serial_dev(uint8_t instance)
{
    drv_serial_t *serial = serial_slot(instance);
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
        /*
         * 这里已经读过 DR，ORE/FE/NE/PE 被清除。DMA 模式的 circular
         * stream 仍在运行，不需要在 USART ISR 里 abort/重启（那会引入
         * HAL 阻塞式 abort）。真正的 DMA 传输错误由 DMA IRQ 的
         * HAL_UART_ErrorCallback 处理。
         */
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
