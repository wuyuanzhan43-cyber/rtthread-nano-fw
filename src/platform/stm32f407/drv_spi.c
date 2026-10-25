/*
 * drv_spi.c - STM32 SPI 实例表驱动。
 *
 * 层级：src/platform/stm32f407
 * 职责：
 *   1. 从板级描述符配置 SCK/MOSI/MISO 复用引脚；
 *   2. 计算 PCLK 分频并初始化 HAL SPI；
 *   3. 按需初始化 TX/RX DMA 并 claim stream；
 *   4. 实现 configure/transfer/async/wait/abort 类 ops；
 *   5. 注册 spi1..N 设备。
 *
 * 上下文：
 *   - configure/transfer/async/wait/abort：任务上下文；
 *   - HAL_SPI_*CpltCallback/ErrorCallback、SPIx_IRQHandler：中断上下文。
 *
 * 依赖：drv_spi.h、fw_config.h、board_config.h、stm32f407_map.h、rthw.h。
 *
 * SPI 参数：
 *   - 主模式、8 位、软件 NSS；
 *   - 模式 0（CPOL=0, CPHA=0）作为默认；
 *   - MSB first；
 *   - 频率由 stm32f407_spi_prescaler 选择不超过 max_hz 的最小分频。
 *
 * DMA 设计：
 *   - 传输使用 CONFIG 的 caller buffer，不做 staging；
 *   - HAL 完成/错误回调只 release 信号量，由任务侧 wait 消费；
 *   - 回调通过 active 标志保证一次传输最多完成一次；
 *   - 同步长度 >= DRV_SPI_DMA_MIN_BYTES 时内部走 async + wait，
 *     短命令（LCD 的 1 字节命令）仍保持 HAL 轮询，避免多余调度。
 *   - 1LINE（use_miso=false）只支持 TX DMA；RX DMA 需要 2LINES 且
 *     板级分配了 RX route。HAL 的 RX DMA 还要求 hdmatx 已初始化，
 *     因此 RX 方向必须与 TX 同时存在。
 */
#include "drv_spi.h"
#include "fw_config.h"
#include "board_config.h"
#include "stm32f407_map.h"
#include <rthw.h>

/* 低于该长度的同步传输仍走轮询；只影响同步 API。 */
#define DRV_SPI_DMA_MIN_BYTES 16U

/*
 * 毫秒超时到 HAL 超时的转换。
 *   UINT32_MAX -> HAL_MAX_DELAY（永久等待）；
 *   0          -> 1（HAL 不接受 0，给最小非零值）；
 *   其他       -> 原值（HAL 以毫秒为单位）。
 */
static uint32_t timeout_to_hal(uint32_t timeout_ms)
{
    if (timeout_ms == UINT32_MAX) return HAL_MAX_DELAY;
    if (timeout_ms == 0U) return 1U;
    return timeout_ms;
}

#if DRV_SPI_USE_DMA
/*
 * 毫秒到 RT-Thread tick 的转换。
 * 0 由调用者单独处理（trytake）；UINT32_MAX 表示永久等待。
 */
static rt_int32_t ticks_from_ms(uint32_t timeout_ms)
{
    rt_tick_t ticks;
    if (timeout_ms == UINT32_MAX) return RT_WAITING_FOREVER;
    ticks = rt_tick_from_millisecond((rt_int32_t)timeout_ms);
    return ticks == 0U ? 1 : (rt_int32_t)ticks;
}

/* SPI 是否启用了任一方向的 DMA。 */
static bool spi_dma_any(const drv_spi_t *spi)
{
    return spi != NULL && (spi->dma_tx_enabled || spi->dma_rx_enabled);
}

/* 清空可能残留的完成 token，避免下一次传输被误判为已完成。 */
static void spi_dma_drain(drv_spi_t *spi)
{
    while (rt_sem_trytake(&spi->dma_done) == RT_EOK) {
    }
}
#endif /* DRV_SPI_USE_DMA */

/*
 * 配置一个 SPI 复用引脚。
 *
 * SCK/MOSI/MISO 都使用推挽复用、最高速度、无上下拉。
 * 片选不在这里配置：它由具体设备作为普通 GPIO 控制，
 * 这样同一总线上的多个从设备可以共用 SPI。
 */
static fw_status_t configure_pin(uint8_t port_index, uint8_t pin_number,
                                 uint8_t af)
{
    GPIO_TypeDef *port = stm32f407_gpio(port_index);
    GPIO_InitTypeDef init = {0};
    if (port == NULL || pin_number > 15U) return FW_EINVAL;
    if (stm32f407_gpio_clock_enable(port_index) != FW_OK) return FW_EINVAL;
    init.Pin = (uint16_t)(1U << pin_number);
    init.Mode = GPIO_MODE_AF_PP;
    init.Pull = GPIO_NOPULL;
    init.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    init.Alternate = af;
    HAL_GPIO_Init(port, &init);
    return FW_OK;
}

#if DRV_SPI_USE_DMA
/*
 * 初始化一个方向的 DMA 句柄。
 *
 * 顺序与 serial 一致：
 *   1. 设置 Instance（drv_dma_claim 会校验它与 route 一致）；
 *   2. claim 打开 DMA 时钟、NVIC 并记录 owner；
 *   3. HAL_DMA_Init 写寄存器；
 *   4. __HAL_LINKDMA 把句柄挂到 SPI。
 *
 * SPI 的两条 stream 都使用 normal 模式：一次 transfer 对应一次
 * DMA 启动，不存在 UART RX 那种 circular 双缓冲。
 */
static fw_status_t spi_dma_setup_one(drv_spi_t *spi, DMA_HandleTypeDef *hdma,
                                     const drv_dma_route_t *route, bool tx,
                                     const char *owner)
{
    uint32_t channel;

    if (spi == NULL || hdma == NULL || route == NULL) return FW_EINVAL;
    hdma->Instance = stm32f407_dma_stream(route->controller, route->stream);
    if (hdma->Instance == NULL) return FW_EINVAL;
    channel = drv_dma_channel_value(route->channel);
    if (channel == 0xFFFFFFFFU) return FW_EINVAL;

    hdma->Init.Channel = channel;
    hdma->Init.Direction = tx ? DMA_MEMORY_TO_PERIPH : DMA_PERIPH_TO_MEMORY;
    hdma->Init.PeriphInc = DMA_PINC_DISABLE;
    hdma->Init.MemInc = DMA_MINC_ENABLE;
    hdma->Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
    hdma->Init.MemDataAlignment = DMA_MDATAALIGN_BYTE;
    hdma->Init.Mode = DMA_NORMAL;
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

    if (tx) __HAL_LINKDMA(&spi->hal, hdmatx, spi->hdma_tx);
    else __HAL_LINKDMA(&spi->hal, hdmarx, spi->hdma_rx);
    return FW_OK;
}

/* DMA owner 诊断名；用实例号索引，避免在中断路径里格式化字符串。 */
static const char *spi_dma_owner(uint8_t instance, bool tx)
{
    static const char *const tx_names[3] = {
        "spi1.tx", "spi2.tx", "spi3.tx"
    };
    static const char *const rx_names[3] = {
        "spi1.rx", "spi2.rx", "spi3.rx"
    };
    if (instance >= 1U && instance <= 3U)
        return tx ? tx_names[instance - 1U] : rx_names[instance - 1U];
    return tx ? "spi.tx" : "spi.rx";
}

/* 注册失败或重配时释放已 claim 的 stream。 */
static void spi_dma_teardown(drv_spi_t *spi)
{
    if (spi == NULL) return;
    if (spi->dma_tx_enabled) {
        drv_dma_release(&spi->hdma_tx);
        spi->dma_tx_enabled = false;
    }
    if (spi->dma_rx_enabled) {
        drv_dma_release(&spi->hdma_rx);
        spi->dma_rx_enabled = false;
    }
    spi->active = false;
}

/*
 * 按板级描述符初始化 DMA。
 *
 * 未启用时把路由清成 DRV_DMA_NONE，后续 async 直接返回 ENOTSUP；
 * 启用时逐方向 claim。RX 还要求 use_miso=true 且 TX 同时启用
 * （HAL 的 HAL_SPI_Receive_DMA 在 2LINES master 下需要 hdmatx）。
 * 任一方向失败释放已成功的 claim，避免留下半初始化状态。
 */
static fw_status_t spi_dma_setup(drv_spi_t *spi, const drv_spi_hw_t *hw,
                                 const char *name)
{
    fw_status_t result;

    spi->dma_tx_route = hw->dma_tx;
    spi->dma_rx_route = hw->dma_rx;
    spi->dma_tx_enabled = hw->dma_tx_enabled;
    spi->dma_rx_enabled = hw->dma_rx_enabled;
    if (!spi->dma_tx_enabled) {
        spi->dma_tx_route.controller = DRV_DMA_NONE;
        spi->dma_tx_route.stream = DRV_DMA_NONE;
        spi->dma_tx_route.channel = DRV_DMA_NONE;
    }
    if (!spi->dma_rx_enabled) {
        spi->dma_rx_route.controller = DRV_DMA_NONE;
        spi->dma_rx_route.stream = DRV_DMA_NONE;
        spi->dma_rx_route.channel = DRV_DMA_NONE;
    }

    if (!spi_dma_any(spi)) return FW_OK;

    if (spi->dma_rx_enabled && (!hw->use_miso || !spi->dma_tx_enabled))
        return FW_EINVAL;

    if (spi->dma_tx_enabled) {
        result = spi_dma_setup_one(spi, &spi->hdma_tx, &spi->dma_tx_route,
                                   true, spi_dma_owner(hw->instance, true));
        if (result != FW_OK) return result;
    }
    if (spi->dma_rx_enabled) {
        result = spi_dma_setup_one(spi, &spi->hdma_rx, &spi->dma_rx_route,
                                   false, spi_dma_owner(hw->instance, false));
        if (result != FW_OK) {
            spi_dma_teardown(spi);
            return result;
        }
    }

    if (rt_sem_init(&spi->dma_done, name, 0U, RT_IPC_FLAG_FIFO) != RT_EOK) {
        spi_dma_teardown(spi);
        return FW_EIO;
    }

    /*
     * HAL 的 DMA 传输会打开 SPI_IT_ERR；没有 NVIC 入口时
     * OVR/MODF/FRE 只能靠超时发现。这里统一使能 SPI 中断，
     * 优先级与 DMA 相同：SPI 错误处理只做 abort/回调，不会长期占用。
     */
    {
        IRQn_Type irq = stm32f407_spi_irq(hw->instance);
        if ((int)irq < 0) {
            spi_dma_teardown(spi);
            return FW_EINVAL;
        }
        HAL_NVIC_SetPriority(irq, BOARD_DMA_IRQ_PRIORITY, 0U);
        HAL_NVIC_EnableIRQ(irq);
    }
    return FW_OK;
}

#else /* !DRV_SPI_USE_DMA */

/*
 * 无 DMA 的构建把初始化和清理折成 no-op；调用点保持同一份代码。
 * async/wait/abort 由下面的桩返回 FW_ENOTSUP，类层不会置 busy。
 */
static fw_status_t spi_dma_setup(drv_spi_t *spi, const drv_spi_hw_t *hw,
                                 const char *name)
{
    (void)spi;
    (void)hw;
    (void)name;
    return FW_OK;
}

static void spi_dma_teardown(drv_spi_t *spi)
{
    (void)spi;
}

#endif /* DRV_SPI_USE_DMA */

/*
 * 应用 SPI 配置。
 *
 * 参数：
 *   max_hz    - 频率上限，驱动向下选择分频；
 *   mode      - 0..3，bit1=CPOL，bit0=CPHA：
 *                 0: 空闲低、第一边沿采样
 *                 1: 空闲低、第二边沿采样
 *                 2: 空闲高、第一边沿采样
 *                 3: 空闲高、第二边沿采样
 *   bit_order - 0=LSB first，1=MSB first。
 *
 * 修改配置会重新调用 HAL_SPI_Init，并重新使能外设。
 * 有传输在途或 DMA 状态不干净时拒绝重配，避免 HAL 改写寄存器
 * 与 DMA 搬运重叠；DMA link 由 __HAL_LINKDMA 建立，Init 不会清除。
 */
static int spi_configure(fw_dev_t *dev, const fw_spi_config_t *config)
{
    drv_spi_t *spi = FW_CONTAINER_OF(dev, drv_spi_t, base.dev);
    HAL_StatusTypeDef status;
    if (spi == NULL || config == NULL || config->max_hz == 0U)
        return FW_EINVAL;
    if (spi->hal.State != HAL_SPI_STATE_READY) return FW_EBUSY;
#if DRV_SPI_USE_DMA
    if (spi->active) return FW_EBUSY;
    if (spi->dma_tx_enabled && spi->hdma_tx.State != HAL_DMA_STATE_READY)
        return FW_EBUSY;
    if (spi->dma_rx_enabled && spi->hdma_rx.State != HAL_DMA_STATE_READY)
        return FW_EBUSY;
#endif

    spi->hal.Init.BaudRatePrescaler =
        stm32f407_spi_prescaler(spi->pclk, config->max_hz);
    spi->hal.Init.CLKPolarity = (config->mode & 2) != 0
                                    ? SPI_POLARITY_HIGH : SPI_POLARITY_LOW;
    spi->hal.Init.CLKPhase = (config->mode & 1) != 0
                                 ? SPI_PHASE_2EDGE : SPI_PHASE_1EDGE;
    spi->hal.Init.FirstBit = config->bit_order != 0
                                 ? SPI_FIRSTBIT_MSB : SPI_FIRSTBIT_LSB;
    status = HAL_SPI_Init(&spi->hal);
    if (status != HAL_OK) return FW_EIO;
    __HAL_SPI_ENABLE(&spi->hal);
    return FW_OK;
}

/* ---------------------------------------------------------------- DMA 状态 */

#if DRV_SPI_USE_DMA

/*
 * 一次传输的完成/错误入口，只在 ISR 调用。
 *
 * active 从 true 变 false 的那一刻 release 一次信号量；迟到的
 * 第二个错误回调（HAL 在 TX/RX 两个 abort 回调里可能各调一次）
 * 会因为 active==false 直接返回，不会污染下一次传输的结果。
 */
static void spi_dma_complete_isr(drv_spi_t *spi, int result)
{
    if (spi == NULL || !spi->active) return;
    spi->active = false;
    spi->dma_result = result;
    (void)rt_sem_release(&spi->dma_done);
}

/*
 * 停止并清理在途 DMA。
 *
 * 顺序要求：
 *   1. 先清 active，抑制 abort 过程中产生的迟到回调；
 *   2. 调用 HAL_SPI_Abort；不能预先清 DMAEN，否则 HAL 会跳过
 *      HAL_DMA_Abort，stream 仍在搬运；
 *   3. 如果 HAL 因某个 stream 已 READY 而返回错误，State/CR2 可能
 *      仍脏，用 HAL_SPI_DMAStop 做一次兜底；
 *   4. 关闭 ERRIE 并 drain 信号量；
 *   5. 确认两个已启用 handle 都回到 READY，否则报告 EIO。
 */
static fw_status_t spi_dma_cleanup(drv_spi_t *spi)
{
    if (spi == NULL) return FW_EINVAL;

    spi->active = false;
    spi->dma_result = FW_EIO;
    (void)HAL_SPI_Abort(&spi->hal);
    if (spi->hal.State != HAL_SPI_STATE_READY ||
        (spi->hal.Instance->CR2 &
         (SPI_CR2_TXDMAEN | SPI_CR2_RXDMAEN)) != 0U) {
        (void)HAL_SPI_DMAStop(&spi->hal);
    }
    __HAL_SPI_DISABLE_IT(&spi->hal, SPI_IT_ERR);
    spi_dma_drain(spi);

    if (spi->dma_tx_enabled && spi->hdma_tx.State != HAL_DMA_STATE_READY)
        return FW_EIO;
    if (spi->dma_rx_enabled && spi->hdma_rx.State != HAL_DMA_STATE_READY)
        return FW_EIO;
    return FW_OK;
}

/*
 * HAL 完成/错误回调。
 *
 * 全双工由 TX stream 触发 TxRxCplt；只发/只收分别走 Tx/RxCplt。
 * DMA 传输错误或 SPI OVR/MODF/FRE 都走 ErrorCallback。
 */
void HAL_SPI_TxCpltCallback(SPI_HandleTypeDef *hspi)
{
    spi_dma_complete_isr(FW_CONTAINER_OF(hspi, drv_spi_t, hal), FW_OK);
}

void HAL_SPI_RxCpltCallback(SPI_HandleTypeDef *hspi)
{
    spi_dma_complete_isr(FW_CONTAINER_OF(hspi, drv_spi_t, hal), FW_OK);
}

void HAL_SPI_TxRxCpltCallback(SPI_HandleTypeDef *hspi)
{
    spi_dma_complete_isr(FW_CONTAINER_OF(hspi, drv_spi_t, hal), FW_OK);
}

void HAL_SPI_ErrorCallback(SPI_HandleTypeDef *hspi)
{
    spi_dma_complete_isr(FW_CONTAINER_OF(hspi, drv_spi_t, hal), FW_EIO);
}

/*
 * 启动异步传输。
 *
 * 启动前检查硬件状态而不是只信任软件标志：错误路径可能留下
 * HAL_DMA_STATE_ABORT，必须等它回到 READY 才能再次启动。
 * 缓冲区必须位于 DMA 可访问的主 SRAM（CCM 会被拒绝）。
 */
static int spi_transfer_async(fw_dev_t *dev, const uint8_t *tx, uint8_t *rx,
                              size_t length)
{
    drv_spi_t *spi = FW_CONTAINER_OF(dev, drv_spi_t, base.dev);
    HAL_StatusTypeDef status;

    if (spi == NULL || length == 0U || length > FW_SPI_MAX_TRANSFER)
        return FW_EINVAL;
    if (tx == NULL && rx == NULL) return FW_EINVAL;
    if (!spi->use_miso && rx != NULL) return FW_ENOTSUP;
    if (!spi_dma_any(spi)) return FW_ENOTSUP;
    if (tx != NULL && !spi->dma_tx_enabled) return FW_ENOTSUP;
    if (rx != NULL && !spi->dma_rx_enabled) return FW_ENOTSUP;
    if (spi->active) return FW_EBUSY;
    if (spi->hal.State != HAL_SPI_STATE_READY) return FW_EBUSY;
    if (tx != NULL && spi->hdma_tx.State != HAL_DMA_STATE_READY)
        return FW_EBUSY;
    if (rx != NULL && spi->hdma_rx.State != HAL_DMA_STATE_READY)
        return FW_EBUSY;
    if (tx != NULL && !drv_dma_ptr_ok(tx, length)) return FW_EINVAL;
    if (rx != NULL && !drv_dma_ptr_ok(rx, length)) return FW_EINVAL;

    spi_dma_drain(spi);
    spi->dma_result = FW_EIO;
    spi->active = true;

    if (tx != NULL && rx != NULL) {
        status = HAL_SPI_TransmitReceive_DMA(&spi->hal, (uint8_t *)tx, rx,
                                             (uint16_t)length);
    } else if (tx != NULL) {
        status = HAL_SPI_Transmit_DMA(&spi->hal, (uint8_t *)tx,
                                      (uint16_t)length);
    } else {
        status = HAL_SPI_Receive_DMA(&spi->hal, rx, (uint16_t)length);
    }

    if (status != HAL_OK) {
        spi->active = false;
        (void)spi_dma_cleanup(spi);
        return status == HAL_BUSY ? FW_EBUSY : FW_EIO;
    }
    return (int)length;
}

/*
 * 等待异步传输结束。
 *
 * 超时只报告 FW_ETIMEOUT，不在这里 abort：类层需要知道
 * “传输仍在途”并负责 abort 与 busy 状态。timeout==0 使用
 * trytake，返回 FW_EAGAIN 让调用者继续轮询。
 */
static int spi_transfer_wait(fw_dev_t *dev, uint32_t timeout_ms)
{
    drv_spi_t *spi = FW_CONTAINER_OF(dev, drv_spi_t, base.dev);
    rt_err_t error;
    int result;

    if (spi == NULL) return FW_EINVAL;
    if (!spi_dma_any(spi)) return FW_ENOTSUP;

    if (timeout_ms == 0U) {
        if (rt_sem_trytake(&spi->dma_done) != RT_EOK) return FW_EAGAIN;
    } else {
        error = rt_sem_take(&spi->dma_done, ticks_from_ms(timeout_ms));
        if (error != RT_EOK) {
            /*
             * tick 超时与 ISR release 可能同时发生：再 trytake 一次，
             * 拿到 token 就按真实结果返回，避免把已完成报成超时。
             */
            if (rt_sem_trytake(&spi->dma_done) != RT_EOK) return FW_ETIMEOUT;
        }
    }

    result = spi->dma_result;
    if (result != FW_OK) {
        /*
         * 错误路径可能只 abort 了一条 stream（HAL 对 ready 的 stream
         * 返回 error），在任务上下文做一次阻塞清理，避免下一次传输
         * 在 HAL_DMA_STATE_ABORT 上卡住。
         *
         * 清理失败用 FW_EBUSY 而不是 FW_EIO 上报：类层据此保留 busy，
         * 让调用者还能通过 abort API 再次尝试清理。
         */
        if (spi_dma_cleanup(spi) != FW_OK) return FW_EBUSY;
    }
    return result;
}

/* 取消在途传输；未在途时也做一次幂等清理。 */
static int spi_transfer_abort(fw_dev_t *dev)
{
    drv_spi_t *spi = FW_CONTAINER_OF(dev, drv_spi_t, base.dev);
    if (spi == NULL) return FW_EINVAL;
    if (!spi_dma_any(spi)) return FW_ENOTSUP;
    return spi_dma_cleanup(spi);
}

#else /* !DRV_SPI_USE_DMA */

/*
 * 无 DMA 构建：类层仍能调用这三个 ops，但直接报告不支持。
 * 类层收到负值不会置 busy，同步 transfer 继续走轮询。
 */
static int spi_transfer_async(fw_dev_t *dev, const uint8_t *tx, uint8_t *rx,
                              size_t length)
{
    (void)dev;
    (void)tx;
    (void)rx;
    (void)length;
    return FW_ENOTSUP;
}

static int spi_transfer_wait(fw_dev_t *dev, uint32_t timeout_ms)
{
    (void)dev;
    (void)timeout_ms;
    return FW_ENOTSUP;
}

static int spi_transfer_abort(fw_dev_t *dev)
{
    (void)dev;
    return FW_ENOTSUP;
}

#endif /* DRV_SPI_USE_DMA */

/* ---------------------------------------------------------------- 传输入口 */

/*
 * 同步传输。
 *
 * 三种模式：
 *   tx && rx   -> 全双工；
 *   tx only    -> 只发（LCD 写命令/像素）；
 *   rx only    -> 只收。
 *
 * 启用 DMA 且长度 >= DRV_SPI_DMA_MIN_BYTES 时内部走 async + wait；
 * 否则保持 HAL 轮询。timeout==0 在 DMA 路径按 1 tick 处理，保证
 * 函数返回时硬件已结束或已 abort，调用者可以安全释放 CS。
 */
static int spi_transfer(fw_dev_t *dev, const uint8_t *tx, uint8_t *rx,
                        size_t length, uint32_t timeout_ms)
{
    drv_spi_t *spi = FW_CONTAINER_OF(dev, drv_spi_t, base.dev);
    HAL_StatusTypeDef status;

    if (spi == NULL || length == 0U || length > FW_SPI_MAX_TRANSFER)
        return FW_EINVAL;
    if (tx == NULL && rx == NULL) return FW_EINVAL;
    if (!spi->use_miso && rx != NULL) return FW_ENOTSUP;

#if DRV_SPI_USE_DMA
    if (spi_dma_any(spi) && length >= DRV_SPI_DMA_MIN_BYTES &&
        (tx == NULL || spi->dma_tx_enabled) &&
        (rx == NULL || spi->dma_rx_enabled)) {
        int started = spi_transfer_async(dev, tx, rx, length);
        int finished;
        if (started < 0) return started;
        finished = spi_transfer_wait(dev, timeout_ms == 0U ? 1U : timeout_ms);
        if (finished == FW_OK) return (int)length;
        if (finished == FW_ETIMEOUT) {
            return spi_transfer_abort(dev) == FW_OK ? FW_ETIMEOUT : FW_EIO;
        }
        return finished;
    }
#endif

    if (tx != NULL && rx != NULL) {
        status = HAL_SPI_TransmitReceive(&spi->hal, (uint8_t *)tx, rx,
                                         (uint16_t)length,
                                         timeout_to_hal(timeout_ms));
    } else if (tx != NULL) {
        status = HAL_SPI_Transmit(&spi->hal, (uint8_t *)tx, (uint16_t)length,
                                  timeout_to_hal(timeout_ms));
    } else {
        status = HAL_SPI_Receive(&spi->hal, rx, (uint16_t)length,
                                 timeout_to_hal(timeout_ms));
    }
    return status == HAL_OK ? (int)length : FW_EIO;
}

/* SPI 驱动 ops。 */
static const fw_spi_ops_t drv_spi_ops = {
    spi_configure,
    spi_transfer,
    spi_transfer_async,
    spi_transfer_wait,
    spi_transfer_abort
};

/*
 * 初始化一个 SPI 实例。
 *
 * 步骤：
 *   1. 校验实例编号 1..3；
 *   2. 配置 SCK/MOSI，必要时配置 MISO；
 *   3. 使能 SPI 时钟，计算该实例的 PCLK；
 *   4. 填写 HAL 初始化参数（主模式、8 位、软件 NSS）；
 *   5. HAL_SPI_Init + 使能；
 *   6. 初始化 DMA（若启用）、信号量和 SPI 错误中断；
 *   7. 填写驱动描述并注册 spi 类设备。
 *
 * 初始配置保存为 max_hz、mode=0、bit_order=1（MSB）；
 * 设备可以在运行期通过 ioctl 修改。
 */
static fw_status_t spi_init_instance(drv_spi_t *spi, const char *name,
                                     const drv_spi_hw_t *hw)
{
    fw_spi_config_t config;
    fw_status_t result;
    SPI_InitTypeDef *init;
    uint8_t instance;

    if (spi == NULL || name == NULL || hw == NULL) return FW_EINVAL;
    instance = hw->instance;
    if (instance < 1U || instance > 3U || hw->max_hz == 0U)
        return FW_EINVAL;

    result = configure_pin(hw->sck_port, hw->sck_pin, hw->sck_af);
    if (result != FW_OK) return result;
    result = configure_pin(hw->mosi_port, hw->mosi_pin, hw->mosi_af);
    if (result != FW_OK) return result;
    if (hw->use_miso) {
        result = configure_pin(hw->miso_port, hw->miso_pin, hw->miso_af);
        if (result != FW_OK) return result;
    }

    stm32f407_spi_clock_enable(instance);
    spi->instance = instance;
    spi->pclk = stm32f407_spi_pclk(instance);
    spi->use_miso = hw->use_miso;
    spi->hal.Instance = stm32f407_spi(instance);
    init = &spi->hal.Init;
    init->Mode = SPI_MODE_MASTER;
    init->Direction = hw->use_miso ? SPI_DIRECTION_2LINES
                                   : SPI_DIRECTION_1LINE;
    init->DataSize = SPI_DATASIZE_8BIT;
    init->CLKPolarity = SPI_POLARITY_LOW;
    init->CLKPhase = SPI_PHASE_1EDGE;
    init->NSS = SPI_NSS_SOFT;
    init->BaudRatePrescaler =
        stm32f407_spi_prescaler(spi->pclk, hw->max_hz);
    init->FirstBit = SPI_FIRSTBIT_MSB;
    init->TIMode = SPI_TIMODE_DISABLE;
    init->CRCCalculation = SPI_CRCCALCULATION_DISABLE;
    init->CRCPolynomial = 10U;
    if (HAL_SPI_Init(&spi->hal) != HAL_OK) return FW_EIO;
    __HAL_SPI_ENABLE(&spi->hal);

#if DRV_SPI_USE_DMA
    spi->active = false;
    spi->dma_result = FW_EIO;
    spi->dma_tx_enabled = false;
    spi->dma_rx_enabled = false;
#endif

    result = spi_dma_setup(spi, hw, name);
    if (result != FW_OK) return result;

    config.max_hz = hw->max_hz;
    config.mode = 0;
    config.bit_order = 1; /* MSB first，LCD 常用 */

    spi->drv.type = FW_DEV_TYPE_SPI;
    spi->drv.ops = &drv_spi_ops;
    spi->drv.data = spi;
    result = fw_spi_register(&spi->base, name, &spi->drv, &config);
    if (result != FW_OK) spi_dma_teardown(spi);
    return result;
}

/*
 * 静态实例定义。
 * 只有 Kconfig 启用对应 SPI 时才分配结构体；关闭的实例不占 RAM。
 */
#if defined(FW_USING_SPI1) && FW_USING_SPI1
static drv_spi_t spi1;
#endif
#if defined(FW_USING_SPI2) && FW_USING_SPI2
static drv_spi_t spi2;
#endif
#if defined(FW_USING_SPI3) && FW_USING_SPI3
static drv_spi_t spi3;
#endif

/*
 * DMA 模式下的 SPI 错误中断入口。
 *
 * HAL 的 DMA API 会打开 SPI_IT_ERR；没有这些入口时 OVR/MODF/FRE
 * 不会被处理，只能等超时。只在对应实例启用 DMA 时定义，其余情况
 * 落到启动文件的弱符号。
 */
#if defined(FW_USING_SPI1) && FW_USING_SPI1 && \
    defined(FW_USING_SPI1_DMA) && FW_USING_SPI1_DMA
void SPI1_IRQHandler(void)
{
    rt_interrupt_enter();
    HAL_SPI_IRQHandler(&spi1.hal);
    rt_interrupt_leave();
}
#endif

#if defined(FW_USING_SPI2) && FW_USING_SPI2 && \
    defined(FW_USING_SPI2_DMA) && FW_USING_SPI2_DMA
void SPI2_IRQHandler(void)
{
    rt_interrupt_enter();
    HAL_SPI_IRQHandler(&spi2.hal);
    rt_interrupt_leave();
}
#endif

#if defined(FW_USING_SPI3) && FW_USING_SPI3 && \
    defined(FW_USING_SPI3_DMA) && FW_USING_SPI3_DMA
void SPI3_IRQHandler(void)
{
    rt_interrupt_enter();
    HAL_SPI_IRQHandler(&spi3.hal);
    rt_interrupt_leave();
}
#endif

static drv_spi_t *spi_slot(uint8_t instance)
{
    switch (instance) {
#if defined(FW_USING_SPI1) && FW_USING_SPI1
    case 1: return &spi1;
#endif
#if defined(FW_USING_SPI2) && FW_USING_SPI2
    case 2: return &spi2;
#endif
#if defined(FW_USING_SPI3) && FW_USING_SPI3
    case 3: return &spi3;
#endif
    default: return NULL;
    }
}

static const char *spi_name(uint8_t instance)
{
    switch (instance) {
    case 1: return "spi1";
    case 2: return "spi2";
    case 3: return "spi3";
    default: return NULL;
    }
}

/*
 * 按板级描述符注册 SPI。
 * 描述符表由 board.c 从 board_config.h 转换而来。
 */
fw_status_t drv_spi_register_all(const drv_spi_hw_t *hw, size_t count)
{
    fw_status_t result;
    size_t i;

    if (count == 0U) return FW_OK;
    if (hw == NULL) return FW_EINVAL;

    for (i = 0U; i < count; ++i) {
        drv_spi_t *slot = spi_slot(hw[i].instance);
        const char *name = spi_name(hw[i].instance);

        if (slot == NULL || name == NULL) return FW_ENOTSUP;
        result = spi_init_instance(slot, name, &hw[i]);
        if (result != FW_OK) return result;
    }
    return FW_OK;
}
