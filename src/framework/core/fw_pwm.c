/*
 * fw_pwm.c - PWM 设备类的通用实现。
 *
 * 层级：src/framework/core
 * 职责：
 *   1. 校验频率、通道和千分比；
 *   2. 把 configure/set_duty/start/stop 转发到驱动；
 *   3. 保存频率配置副本。
 *
 * 上下文：任务上下文；configure 可能重算定时器参数，但不阻塞。
 * 依赖：fw/pwm.h。
 *
 * 设计说明：
 *   类层不包含 HAL；通道号在类层是 0..3，驱动负责映射到
 *   TIM_CHANNEL_1..4。start/stop 是定时器级操作，不是单通道操作。
 */
#include "fw/pwm.h"

static const fw_pwm_ops_t *pwm_ops(const fw_dev_t *dev)
{
    if (dev == NULL || dev->drv == NULL || dev->drv->ops == NULL) return NULL;
    return (const fw_pwm_ops_t *)dev->drv->ops;
}

static bool channel_ok(int channel)
{
    return channel >= 0 && channel < FW_PWM_CHANNELS;
}

static bool duty_ok(uint32_t permille)
{
    return permille <= FW_PWM_DUTY_MAX;
}

/*
 * ioctl：
 *   SET_CONFIG (args=fw_pwm_config_t*)；
 *   GET_CONFIG (args=fw_pwm_config_t*)；
 *   SET_DUTY   (off=channel, args=uint32_t*)；
 *   START/STOP (全局，off 忽略)。
 */
static int pwm_ioctl(fw_dev_t *dev, int off, int cmd, void *args)
{
    fw_pwm_t *pwm = FW_CONTAINER_OF(dev, fw_pwm_t, dev);
    const fw_pwm_ops_t *ops = pwm_ops(dev);

    if (cmd == FW_CTL_PWM_SET_CONFIG) {
        fw_pwm_config_t config;
        if (args == NULL || ops == NULL || ops->configure == NULL)
            return FW_EINVAL;
        config = *(const fw_pwm_config_t *)args;
        if (config.frequency_hz == 0U) return FW_EINVAL;
        {
            int result = ops->configure(dev, &config);
            if (result < 0) return result;
        }
        pwm->config = config;
        return (int)sizeof(config);
    }

    if (cmd == FW_CTL_PWM_GET_CONFIG) {
        if (args == NULL) return FW_EINVAL;
        *(fw_pwm_config_t *)args = pwm->config;
        return (int)sizeof(pwm->config);
    }

    if (cmd == FW_CTL_PWM_SET_DUTY) {
        uint32_t permille;
        if (args == NULL || ops == NULL || ops->set_duty == NULL ||
            !channel_ok(off))
            return FW_EINVAL;
        permille = *(const uint32_t *)args;
        if (!duty_ok(permille)) return FW_EINVAL;
        return ops->set_duty(dev, off, permille);
    }

    if (cmd == FW_CTL_PWM_START) {
        if (ops == NULL || ops->start == NULL) return FW_ENOTSUP;
        return ops->start(dev);
    }

    if (cmd == FW_CTL_PWM_STOP) {
        if (ops == NULL || ops->stop == NULL) return FW_ENOTSUP;
        return ops->stop(dev);
    }
    return FW_ENOTSUP;
}

static const fw_dev_ops_t pwm_dev_ops = {
    NULL,
    NULL,
    NULL,
    NULL,
    pwm_ioctl,
    NULL
};

fw_status_t fw_pwm_register(fw_pwm_t *pwm, const char *name,
                            const fw_drv_t *drv,
                            const fw_pwm_config_t *config)
{
    const fw_pwm_ops_t *ops;
    fw_status_t result;

    if (pwm == NULL || name == NULL || drv == NULL || drv->ops == NULL ||
        config == NULL || config->frequency_hz == 0U) return FW_EINVAL;
    if (drv->type != FW_DEV_TYPE_PWM) return FW_EINVAL;

    ops = (const fw_pwm_ops_t *)drv->ops;
    if (ops->configure == NULL || ops->set_duty == NULL ||
        ops->start == NULL || ops->stop == NULL)
        return FW_EINVAL;

    result = fw_dev_register(&pwm->dev, name, FW_DEV_TYPE_PWM, drv);
    if (result != FW_OK) return result;

    pwm->dev.ops = &pwm_dev_ops;
    pwm->config = *config;
    return FW_OK;
}

fw_pwm_t *fw_pwm_find(const char *name)
{
    fw_dev_t *dev = fw_dev_find(name);
    if (dev == NULL || dev->type != FW_DEV_TYPE_PWM) return NULL;
    return FW_CONTAINER_OF(dev, fw_pwm_t, dev);
}

fw_status_t fw_pwm_set_duty(fw_dev_t *dev, int channel, uint32_t permille)
{
    const fw_pwm_ops_t *ops = pwm_ops(dev);
    if (ops == NULL || ops->set_duty == NULL) return FW_ENOTSUP;
    if (!channel_ok(channel) || !duty_ok(permille)) return FW_EINVAL;
    {
        int result = ops->set_duty(dev, channel, permille);
        if (result < 0) fw_dev_note_error(dev);
        return (fw_status_t)result;
    }
}

fw_status_t fw_pwm_start(fw_dev_t *dev)
{
    const fw_pwm_ops_t *ops = pwm_ops(dev);
    int result;
    if (ops == NULL || ops->start == NULL) return FW_ENOTSUP;
    result = ops->start(dev);
    if (result < 0) fw_dev_note_error(dev);
    return (fw_status_t)result;
}

fw_status_t fw_pwm_stop(fw_dev_t *dev)
{
    const fw_pwm_ops_t *ops = pwm_ops(dev);
    int result;
    if (ops == NULL || ops->stop == NULL) return FW_ENOTSUP;
    result = ops->stop(dev);
    if (result < 0) fw_dev_note_error(dev);
    return (fw_status_t)result;
}

fw_status_t fw_pwm_get_config(fw_dev_t *dev, fw_pwm_config_t *config)
{
    int result = fw_dev_ioctl(dev, 0, FW_CTL_PWM_GET_CONFIG, config);
    return result < 0 ? (fw_status_t)result : FW_OK;
}
