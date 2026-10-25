/*
 * fw/pwm.h - mr 风格 PWM 设备类。
 *
 * 层级：src/framework/core
 * 职责：
 *   1. 每个定时器对应一个设备：pwm4 对应 TIM4；
 *   2. 频率属于定时器级配置，占空比属于通道级配置；
 *   3. 提供全局 start/stop 和逐通道 set_duty。
 *
 * 上下文：任务上下文；配置和启动均不阻塞。
 * 依赖：fw/dev.h。
 *
 * 通道语义：
 *   - 框架通道号 0..3，对应 TIM_CHANNEL_1..4；
 *   - 占空比使用千分比 0..1000，避免绑定具体 ARR；
 *   - start/stop 是全局语义：停机会关闭整个定时器。
 */
#ifndef FW_PWM_H
#define FW_PWM_H

#include "fw/dev.h"

/* ioctl 命令。SET_DUTY 的 off 是通道，args 是 uint32_t* 千分比。 */
#define FW_CTL_PWM_SET_CONFIG (0x08 << 24)
#define FW_CTL_PWM_GET_CONFIG (-(0x08 << 24))
#define FW_CTL_PWM_SET_DUTY (0x09 << 24)
#define FW_CTL_PWM_START (0x0a << 24)
#define FW_CTL_PWM_STOP (0x0b << 24)

/* 框架通道数与占空比上限。 */
#define FW_PWM_CHANNELS 4
#define FW_PWM_DUTY_MAX 1000U

/* 定时器级配置；frequency_hz 必须大于 0。 */
typedef struct {
    uint32_t frequency_hz;
} fw_pwm_config_t;

/*
 * PWM 驱动操作集。
 *
 * configure - 重算 PSC/ARR 并应用频率；已启动时返回 FW_EBUSY；
 * set_duty  - 设置某通道千分比，可在运行期调用；
 * start     - 启动所有已配置通道；
 * stop      - 停止定时器输出。
 */
typedef struct {
    int (*configure)(fw_dev_t *dev, const fw_pwm_config_t *config);
    int (*set_duty)(fw_dev_t *dev, int channel, uint32_t permille);
    int (*start)(fw_dev_t *dev);
    int (*stop)(fw_dev_t *dev);
} fw_pwm_ops_t;

typedef struct {
    fw_dev_t dev;
    fw_pwm_config_t config;
} fw_pwm_t;

fw_status_t fw_pwm_register(fw_pwm_t *pwm, const char *name,
                            const fw_drv_t *drv,
                            const fw_pwm_config_t *config);

fw_pwm_t *fw_pwm_find(const char *name);

/* 设置通道占空比（0..1000‰）。 */
fw_status_t fw_pwm_set_duty(fw_dev_t *dev, int channel, uint32_t permille);

/* 全局启动/停止，重复调用安全。 */
fw_status_t fw_pwm_start(fw_dev_t *dev);
fw_status_t fw_pwm_stop(fw_dev_t *dev);

/* 读取当前软件配置。 */
fw_status_t fw_pwm_get_config(fw_dev_t *dev, fw_pwm_config_t *config);

#endif
