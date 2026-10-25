/*
 * drv_pwm.c - STM32 TIM4 PWM 驱动。
 *
 * 层级：src/platform/stm32f407
 * 职责：
 *   1. 配置 PD12..PD15 的 AF2 复用；
 *   2. 按 TIM4 时钟计算 PSC/ARR 并初始化四路 PWM；
 *   3. 实现 configure/set_duty/start/stop；
 *   4. 注册 pwm4。
 *
 * 上下文：任务上下文；不使用中断/DMA。
 * 依赖：drv_pwm.h、fw_config.h、stm32f407_map.h。
 *
 * 定时器时钟：
 *   APB1 分频为 1 时 TIM4CLK=PCLK1，否则为 2*PCLK1。
 * 占空比：
 *   框架通道 0..3 -> TIM_CHANNEL_1..4；千分比 0..1000。
 *   CCR=ARR+1 可实现真正 100%，但 ARR=65535 时 CCR 会溢出 16 位，
 *   因此该边界钳位到 ARR（约 99.998%）。
 */
#include "drv_pwm.h"
#include "fw_config.h"
#include "stm32f407_map.h"

/* 配置一个 PWM 复用引脚（推挽 AF）。 */
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

/* 框架通道 0..3 -> HAL 通道；非法返回 0。 */
static uint32_t hal_channel(int channel)
{
    switch (channel) {
    case 0: return TIM_CHANNEL_1;
    case 1: return TIM_CHANNEL_2;
    case 2: return TIM_CHANNEL_3;
    case 3: return TIM_CHANNEL_4;
    default: return 0U;
    }
}

/*
 * 计算 PSC/ARR。
 *
 *   ticks = TIMCLK / freq
 *   psc   = ceil(ticks / 65536) - 1
 *   arr   = floor(TIMCLK / ((psc+1)*freq)) - 1
 *
 * ARR 必须落在 1..65535；频率过低/过高返回 FW_EINVAL。
 */
static fw_status_t pwm_calc(uint32_t tim_clk, uint32_t freq,
                            uint32_t *psc, uint32_t *arr)
{
    uint64_t ticks;
    uint32_t prescaler;
    uint64_t period;

    if (tim_clk == 0U || freq == 0U || psc == NULL || arr == NULL)
        return FW_EINVAL;
    ticks = (uint64_t)tim_clk / freq;
    if (ticks == 0U) return FW_EINVAL;

    prescaler = (uint32_t)((ticks + 65535U) / 65536U) - 1U;
    if (prescaler > 65535U) return FW_EINVAL;

    period = (uint64_t)tim_clk /
             ((uint64_t)(prescaler + 1U) * (uint64_t)freq);
    /* ARR 必须落在 1..65535；period==1 会得到 ARR=0。 */
    if (period < 2U || period > 65536U) return FW_EINVAL;

    *psc = prescaler;
    *arr = (uint32_t)(period - 1U);
    return FW_OK;
}

/* 配置单个通道；Pulse 由调用者给出。 */
static fw_status_t pwm_config_channel(drv_pwm_t *pwm, uint32_t channel,
                                      uint32_t pulse)
{
    TIM_OC_InitTypeDef oc = {0};
    oc.OCMode = TIM_OCMODE_PWM1;
    oc.Pulse = pulse;
    oc.OCPolarity = TIM_OCPOLARITY_HIGH;
    oc.OCNPolarity = TIM_OCNPOLARITY_HIGH;
    oc.OCFastMode = TIM_OCFAST_DISABLE;
    oc.OCIdleState = TIM_OCIDLESTATE_RESET;
    oc.OCNIdleState = TIM_OCNIDLESTATE_RESET;
    return HAL_TIM_PWM_ConfigChannel(&pwm->hal, &oc, channel) == HAL_OK
               ? FW_OK : FW_EIO;
}

/* 千分比 -> CCR，处理 16 位边界。 */
static uint32_t duty_pulse(uint32_t arr, uint32_t permille)
{
    uint64_t pulse;
    if (permille == 0U) return 0U;
    if (permille >= FW_PWM_DUTY_MAX) {
        return arr == 0xFFFFU ? arr : arr + 1U;
    }
    pulse = ((uint64_t)(arr + 1U) * permille) / FW_PWM_DUTY_MAX;
    return (uint32_t)pulse;
}

/*
 * 应用频率。
 *
 * 已启动时拒绝（返回 EBUSY），避免在输出过程中改写 ARR/PSC。
 * 停止状态下重新 HAL_TIM_PWM_Init，并按保存的千分比恢复 CCR。
 */
static int pwm_configure(fw_dev_t *dev, const fw_pwm_config_t *config)
{
    drv_pwm_t *pwm = FW_CONTAINER_OF(dev, drv_pwm_t, base.dev);
    TIM_Base_InitTypeDef *init;
    uint32_t psc;
    uint32_t arr;
    int channel;

    if (pwm == NULL || config == NULL || config->frequency_hz == 0U)
        return FW_EINVAL;
    if (pwm->started) return FW_EBUSY;
    if (pwm_calc(stm32f407_tim4_clk(), config->frequency_hz, &psc, &arr) !=
        FW_OK)
        return FW_EINVAL;

    init = &pwm->hal.Init;
    init->Prescaler = psc;
    init->CounterMode = TIM_COUNTERMODE_UP;
    init->Period = arr;
    init->ClockDivision = TIM_CLOCKDIVISION_DIV1;
    init->RepetitionCounter = 0U;
    init->AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
    if (HAL_TIM_PWM_Init(&pwm->hal) != HAL_OK) return FW_EIO;

    pwm->arr = arr;
    for (channel = 0; channel < DRV_PWM_MAX_CHANNELS; ++channel) {
        uint32_t hal = hal_channel(channel);
        if (hal == 0U || (pwm->channel_mask & (uint16_t)(1U << channel)) == 0U)
            continue;
        if (pwm_config_channel(pwm, hal,
                               duty_pulse(arr, pwm->duty[channel])) != FW_OK)
            return FW_EIO;
    }
    return FW_OK;
}

/* 设置通道千分比；可在运行期调用。 */
static int pwm_set_duty(fw_dev_t *dev, int channel, uint32_t permille)
{
    drv_pwm_t *pwm = FW_CONTAINER_OF(dev, drv_pwm_t, base.dev);
    uint32_t hal;
    if (pwm == NULL) return FW_EINVAL;
    if (permille > FW_PWM_DUTY_MAX) return FW_EINVAL;
    if (channel < 0 || channel >= DRV_PWM_MAX_CHANNELS) return FW_EINVAL;
    hal = hal_channel(channel);
    if (hal == 0U ||
        (pwm->channel_mask & (uint16_t)(1U << channel)) == 0U)
        return FW_ENOTSUP;

    pwm->duty[channel] = permille;
    __HAL_TIM_SET_COMPARE(&pwm->hal, hal, duty_pulse(pwm->arr, permille));
    return FW_OK;
}

/* 启动所有已配置通道；重复调用安全。 */
static int pwm_start(fw_dev_t *dev)
{
    drv_pwm_t *pwm = FW_CONTAINER_OF(dev, drv_pwm_t, base.dev);
    int channel;
    if (pwm == NULL) return FW_EINVAL;
    if (pwm->started) return FW_OK;

    for (channel = 0; channel < DRV_PWM_MAX_CHANNELS; ++channel) {
        uint32_t hal = hal_channel(channel);
        if (hal == 0U || (pwm->channel_mask & (uint16_t)(1U << channel)) == 0U)
            continue;
        if (HAL_TIM_PWM_Start(&pwm->hal, hal) != HAL_OK) return FW_EIO;
    }
    pwm->started = true;
    return FW_OK;
}

/* 停止定时器输出；重复调用安全。 */
static int pwm_stop(fw_dev_t *dev)
{
    drv_pwm_t *pwm = FW_CONTAINER_OF(dev, drv_pwm_t, base.dev);
    int channel;
    if (pwm == NULL) return FW_EINVAL;
    if (!pwm->started) return FW_OK;

    for (channel = 0; channel < DRV_PWM_MAX_CHANNELS; ++channel) {
        uint32_t hal = hal_channel(channel);
        if (hal == 0U || (pwm->channel_mask & (uint16_t)(1U << channel)) == 0U)
            continue;
        (void)HAL_TIM_PWM_Stop(&pwm->hal, hal);
    }
    pwm->started = false;
    return FW_OK;
}

static const fw_pwm_ops_t drv_pwm_ops = {
    pwm_configure,
    pwm_set_duty,
    pwm_start,
    pwm_stop
};

/* 初始化 TIM4 与四路 PWM；初始化后不启动输出。 */
static fw_status_t pwm_init_instance(drv_pwm_t *pwm, const char *name,
                                     const drv_pwm_hw_t *hw)
{
    fw_pwm_config_t config;
    fw_status_t result;
    uint16_t mask = 0U;
    uint8_t i;

    if (pwm == NULL || name == NULL || hw == NULL) return FW_EINVAL;
    if (hw->instance != 4U || hw->channel_count == 0U ||
        hw->channel_count > DRV_PWM_MAX_CHANNELS || hw->frequency_hz == 0U)
        return FW_EINVAL;

    for (i = 0U; i < hw->channel_count; ++i) {
        uint8_t channel = hw->channels[i].channel;
        if (channel < 1U || channel > 4U) return FW_EINVAL;
        if ((mask & (uint16_t)(1U << (channel - 1U))) != 0U) return FW_EINVAL;
        mask |= (uint16_t)(1U << (channel - 1U));
        result = configure_pin(hw->channels[i].port, hw->channels[i].pin,
                               hw->channels[i].af);
        if (result != FW_OK) return result;
    }

    stm32f407_tim4_clock_enable();
    pwm->instance = 4U;
    pwm->channel_mask = mask;
    pwm->arr = 0U;
    pwm->started = false;
    for (i = 0U; i < DRV_PWM_MAX_CHANNELS; ++i) pwm->duty[i] = 0U;

    pwm->hal.Instance = stm32f407_tim4();
    if (pwm->hal.Instance == NULL) return FW_EINVAL;

    config.frequency_hz = hw->frequency_hz;
    if (pwm_configure(&pwm->base.dev, &config) != FW_OK) return FW_EIO;

    pwm->drv.type = FW_DEV_TYPE_PWM;
    pwm->drv.ops = &drv_pwm_ops;
    pwm->drv.data = pwm;
    return fw_pwm_register(&pwm->base, name, &pwm->drv, &config);
}

#if defined(FW_USING_PWM4) && FW_USING_PWM4
static drv_pwm_t pwm4;
#endif

fw_status_t drv_pwm_register_all(const drv_pwm_hw_t *hw, size_t count)
{
    size_t i;

    if (count == 0U) return FW_OK;
    if (hw == NULL) return FW_EINVAL;

    for (i = 0U; i < count; ++i) {
#if defined(FW_USING_PWM4) && FW_USING_PWM4
        if (hw[i].instance == 4U)
            return pwm_init_instance(&pwm4, "pwm4", &hw[i]);
#endif
        return FW_ENOTSUP;
    }
    return FW_OK;
}
