/*
 * drv_adc.c - STM32 ADC 实例表驱动。
 *
 * 层级：src/platform/stm32f407
 * 职责：
 *   1. 把板级通道引脚配置为模拟输入；
 *   2. 初始化 ADC1 的时钟分频/分辨率；
 *   3. 每次 read 配置通道并做一次软件触发转换；
 *   4. 注册 adc1。
 *
 * 上下文：任务上下文，阻塞轮询；不使用中断/DMA。
 * 依赖：drv_adc.h、fw_config.h、stm32f407_map.h、drv_common.h。
 *
 * ADCCLK：
 *   ADCCLK = PCLK2 / ADC 预分频，F407 上限 36MHz。
 *   HSI 16MHz -> /2 = 8MHz；HSE 84MHz PCLK2 -> /4 = 21MHz。
 *
 * 校准：
 *   当前 V1.8.3 HAL / CMSIS 子集没有提供 ADC 校准 API，首版不调用。
 */
#include "drv_adc.h"
#include "fw_config.h"
#include "stm32f407_map.h"
#include "drv_common.h"

/* 配置一个模拟输入引脚。 */
static fw_status_t configure_pin(uint8_t port_index, uint8_t pin_number)
{
    GPIO_TypeDef *port = stm32f407_gpio(port_index);
    GPIO_InitTypeDef init = {0};
    if (port == NULL || pin_number > 15U) return FW_EINVAL;
    if (stm32f407_gpio_clock_enable(port_index) != FW_OK) return FW_EINVAL;
    init.Pin = (uint16_t)(1U << pin_number);
    init.Mode = GPIO_MODE_ANALOG;
    init.Pull = GPIO_NOPULL;
    init.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(port, &init);
    return FW_OK;
}

/* 选择 ADCCLK 分频，保证 PCLK2/div <= 36MHz。 */
static uint32_t clock_prescaler(void)
{
    uint32_t pclk2 = HAL_RCC_GetPCLK2Freq();
    if (pclk2 / 2U <= 36000000U) return ADC_CLOCK_SYNC_PCLK_DIV2;
    if (pclk2 / 4U <= 36000000U) return ADC_CLOCK_SYNC_PCLK_DIV4;
    if (pclk2 / 6U <= 36000000U) return ADC_CLOCK_SYNC_PCLK_DIV6;
    return ADC_CLOCK_SYNC_PCLK_DIV8;
}

/* 分辨率位数 -> HAL 宏。 */
static fw_status_t resolution_value(uint8_t bits, uint32_t *value)
{
    switch (bits) {
    case 12U: *value = ADC_RESOLUTION_12B; return FW_OK;
    case 10U: *value = ADC_RESOLUTION_10B; return FW_OK;
    case 8U: *value = ADC_RESOLUTION_8B; return FW_OK;
    default: return FW_EINVAL;
    }
}

/* 采样周期 -> HAL 宏。 */
static fw_status_t sample_time_value(uint16_t cycles, uint32_t *value)
{
    switch (cycles) {
    case 3U: *value = ADC_SAMPLETIME_3CYCLES; return FW_OK;
    case 15U: *value = ADC_SAMPLETIME_15CYCLES; return FW_OK;
    case 28U: *value = ADC_SAMPLETIME_28CYCLES; return FW_OK;
    case 56U: *value = ADC_SAMPLETIME_56CYCLES; return FW_OK;
    case 84U: *value = ADC_SAMPLETIME_84CYCLES; return FW_OK;
    case 112U: *value = ADC_SAMPLETIME_112CYCLES; return FW_OK;
    case 144U: *value = ADC_SAMPLETIME_144CYCLES; return FW_OK;
    case 480U: *value = ADC_SAMPLETIME_480CYCLES; return FW_OK;
    default: return FW_EINVAL;
    }
}

/* 通道号 0..15 -> ADC_CHANNEL_n。 */
static uint32_t channel_value(uint8_t channel, bool *ok)
{
    *ok = true;
    switch (channel) {
    case 0U: return ADC_CHANNEL_0;
    case 1U: return ADC_CHANNEL_1;
    case 2U: return ADC_CHANNEL_2;
    case 3U: return ADC_CHANNEL_3;
    case 4U: return ADC_CHANNEL_4;
    case 5U: return ADC_CHANNEL_5;
    case 6U: return ADC_CHANNEL_6;
    case 7U: return ADC_CHANNEL_7;
    case 8U: return ADC_CHANNEL_8;
    case 9U: return ADC_CHANNEL_9;
    case 10U: return ADC_CHANNEL_10;
    case 11U: return ADC_CHANNEL_11;
    case 12U: return ADC_CHANNEL_12;
    case 13U: return ADC_CHANNEL_13;
    case 14U: return ADC_CHANNEL_14;
    case 15U: return ADC_CHANNEL_15;
    default: *ok = false; return 0U;
    }
}

/* 按当前配置重新初始化 ADC；分辨率变化时必须先停止转换。 */
static fw_status_t adc_apply_config(drv_adc_t *adc)
{
    ADC_InitTypeDef *init;
    uint32_t resolution;

    if (resolution_value(adc->resolution_bits, &resolution) != FW_OK)
        return FW_EINVAL;

    (void)HAL_ADC_Stop(&adc->hal);
    init = &adc->hal.Init;
    init->ClockPrescaler = clock_prescaler();
    init->Resolution = resolution;
    init->DataAlign = ADC_DATAALIGN_RIGHT;
    init->ScanConvMode = DISABLE;
    init->EOCSelection = ADC_EOC_SINGLE_CONV;
    init->ContinuousConvMode = DISABLE;
    init->NbrOfConversion = 1U;
    init->DiscontinuousConvMode = DISABLE;
    init->NbrOfDiscConversion = 0U;
    init->ExternalTrigConv = ADC_SOFTWARE_START;
    init->ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
    init->DMAContinuousRequests = DISABLE;
    return HAL_ADC_Init(&adc->hal) == HAL_OK ? FW_OK : FW_EIO;
}

/* 应用 vref/分辨率/采样周期；运行期调用会重新初始化 ADC。 */
static int adc_configure(fw_dev_t *dev, const fw_adc_config_t *config)
{
    drv_adc_t *adc = FW_CONTAINER_OF(dev, drv_adc_t, base.dev);
    uint32_t sample;
    if (adc == NULL || config == NULL) return FW_EINVAL;
    if (sample_time_value(config->sample_cycles, &sample) != FW_OK)
        return FW_EINVAL;
    adc->resolution_bits = config->resolution_bits;
    adc->sample_cycles = config->sample_cycles;
    return adc_apply_config(adc) == FW_OK ? FW_OK : FW_EIO;
}

/*
 * 单次转换：
 *   ConfigChannel -> Start -> Poll -> GetValue
 * 成功时保持 ADC 使能，下一次 Start 不需要重新等待稳定时间；
 * 错误/超时后 Stop，便于下一次恢复。
 */
static int adc_read(fw_dev_t *dev, int channel, uint16_t *raw,
                    uint32_t timeout_ms)
{
    drv_adc_t *adc = FW_CONTAINER_OF(dev, drv_adc_t, base.dev);
    ADC_ChannelConfTypeDef conf = {0};
    HAL_StatusTypeDef status;
    uint32_t sample;
    bool ok;

    if (adc == NULL || raw == NULL || channel < 0 || channel > 15)
        return FW_EINVAL;
    if ((adc->channel_mask & (uint16_t)(1U << channel)) == 0U)
        return FW_ENOTSUP;
    if (sample_time_value(adc->sample_cycles, &sample) != FW_OK)
        return FW_EINVAL;

    conf.Channel = channel_value((uint8_t)channel, &ok);
    if (!ok) return FW_EINVAL;
    conf.Rank = 1U;
    conf.SamplingTime = sample;
    conf.Offset = 0U;

    if (HAL_ADC_ConfigChannel(&adc->hal, &conf) != HAL_OK) return FW_EIO;
    status = HAL_ADC_Start(&adc->hal);
    if (status != HAL_OK) return (int)drv_status_from_hal(status);
    status = HAL_ADC_PollForConversion(&adc->hal,
                                       drv_timeout_to_hal(timeout_ms));
    if (status != HAL_OK) {
        (void)HAL_ADC_Stop(&adc->hal);
        return (int)drv_status_from_hal(status);
    }
    *raw = (uint16_t)HAL_ADC_GetValue(&adc->hal);
    return FW_OK;
}

static const fw_adc_ops_t drv_adc_ops = {
    adc_configure,
    adc_read
};

/* 初始化 ADC1。 */
static fw_status_t adc_init_instance(drv_adc_t *adc, const char *name,
                                     const drv_adc_hw_t *hw)
{
    fw_adc_config_t config;
    fw_status_t result;
    uint16_t mask = 0U;
    uint8_t i;

    if (adc == NULL || name == NULL || hw == NULL) return FW_EINVAL;
    if (hw->instance != 1U || hw->channel_count == 0U ||
        hw->channel_count > DRV_ADC_MAX_CHANNELS || hw->vref_mv == 0U)
        return FW_EINVAL;

    for (i = 0U; i < hw->channel_count; ++i) {
        uint8_t channel = hw->channels[i].channel;
        if (channel > 15U) return FW_EINVAL;
        if ((mask & (uint16_t)(1U << channel)) != 0U) return FW_EINVAL;
        mask |= (uint16_t)(1U << channel);
        result = configure_pin(hw->channels[i].port, hw->channels[i].pin);
        if (result != FW_OK) return result;
    }

    stm32f407_adc_clock_enable();
    adc->instance = 1U;
    adc->channel_mask = mask;
    adc->resolution_bits = hw->resolution_bits;
    adc->sample_cycles = hw->sample_cycles;
    adc->hal.Instance = stm32f407_adc1();
    if (adc->hal.Instance == NULL) return FW_EINVAL;
    result = adc_apply_config(adc);
    if (result != FW_OK) return result;

    config.vref_mv = hw->vref_mv;
    config.resolution_bits = hw->resolution_bits;
    config.sample_cycles = hw->sample_cycles;

    adc->drv.type = FW_DEV_TYPE_ADC;
    adc->drv.ops = &drv_adc_ops;
    adc->drv.data = adc;
    return fw_adc_register(&adc->base, name, &adc->drv, &config);
}

#if defined(FW_USING_ADC1) && FW_USING_ADC1
static drv_adc_t adc1;
#endif

fw_status_t drv_adc_register_all(const drv_adc_hw_t *hw, size_t count)
{
    size_t i;

    if (count == 0U) return FW_OK;
    if (hw == NULL) return FW_EINVAL;

    for (i = 0U; i < count; ++i) {
#if defined(FW_USING_ADC1) && FW_USING_ADC1
        if (hw[i].instance == 1U)
            return adc_init_instance(&adc1, "adc1", &hw[i]);
#endif
        return FW_ENOTSUP;
    }
    return FW_OK;
}
