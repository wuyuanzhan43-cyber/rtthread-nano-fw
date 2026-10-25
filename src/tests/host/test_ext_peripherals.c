/*
 * test_ext_peripherals.c - I2C/ADC/PWM 设备类 Host 测试。
 *
 * 层级：src/tests/host
 * 职责：
 *   1. 验证 I2C 地址/长度校验、原始收发、mem_read/write、probe；
 *   2. 验证 ADC 通道读取、mV 换算和参数校验；
 *   3. 验证 PWM 占空比、启停和运行期重配约束。
 *
 * 上下文：单线程；不模拟 HAL，只验证类层策略和驱动回调路径。
 * 依赖：test_common.h、host_devices.h、fw/{i2c,adc,pwm}.h。
 */
#include "test_common.h"
#include "host_devices.h"
#include "fw/adc.h"
#include "fw/i2c.h"
#include "fw/pwm.h"

/* I2C：配置、原始收发、寄存器读写、探测和错误传播。 */
static void test_i2c_class(void)
{
    fw_host_t host;
    fw_i2c_t *i2c;
    fw_i2c_config_t config = { 400000U };
    fw_i2c_config_t readback = { 0U };
    uint8_t tx[3] = {0x11U, 0x22U, 0x33U};
    uint8_t rx[3] = {0};

    host_init(&host);
    host_register_devices(&host);
    i2c = fw_i2c_find("i2c1");
    CHECK(i2c != NULL);
    if (i2c == NULL) return;

    /* 配置读写。 */
    CHECK(fw_dev_ioctl(&i2c->dev, 0, FW_CTL_I2C_SET_CONFIG, &config) ==
          (int)sizeof(config));
    CHECK(fw_dev_ioctl(&i2c->dev, 0, FW_CTL_I2C_GET_CONFIG, &readback) ==
          (int)sizeof(readback));
    CHECK(readback.clock_hz == 400000U);
    CHECK(host.i2c_config.clock_hz == 400000U);

    /* 单写：off 就是 7 位地址。 */
    CHECK(fw_dev_write(&i2c->dev, 0x3C, tx, 2U, 10U) == 2);
    CHECK(host.i2c_transfers == 1U);
    CHECK(host.i2c_last_addr == 0x3CU);
    CHECK(host.i2c_tx_length == 2U && memcmp(host.i2c_tx, tx, 2U) == 0);

    /* 单读：RX 由 fake 填 0x80.. 模式。 */
    CHECK(fw_dev_read(&i2c->dev, 0x3C, rx, sizeof(rx), 10U) == 3);
    CHECK(rx[0] == 0x80U && rx[2] == 0x82U);

    /* 写后读：返回 tx+rx 总数。 */
    host.i2c_tx_length = 0U;
    CHECK(fw_i2c_transfer(&i2c->dev, 0x20, tx, sizeof(tx), rx, sizeof(rx),
                          10U) == (int)(sizeof(tx) + sizeof(rx)));
    CHECK(host.i2c_last_addr == 0x20U);
    CHECK(host.i2c_tx_length == 3U);

    /* 地址和长度校验。 */
    CHECK(fw_i2c_transfer(&i2c->dev, 0x80, tx, 1U, NULL, 0U, 10U) ==
          FW_EINVAL);
    CHECK(fw_i2c_transfer(&i2c->dev, 0x20, NULL, 0U, NULL, 0U, 10U) ==
          FW_EINVAL);
    CHECK(fw_i2c_transfer(&i2c->dev, 0x20, tx,
                          FW_I2C_MAX_TRANSFER + 1U, NULL, 0U, 10U) ==
          FW_EINVAL);

    /* 寄存器读写：mem_addr_size 只接受 1/2。 */
    CHECK(fw_i2c_mem_write(&i2c->dev, 0x3C, 0x10, 1U, tx, 2U, 10U) == 2);
    CHECK(host.i2c_last_mem_addr == 0x10U);
    CHECK(host.i2c_last_mem_size == 1U);
    CHECK(fw_i2c_mem_read(&i2c->dev, 0x3C, 0x10, 1U, rx, sizeof(rx), 10U) == 3);
    CHECK(rx[0] == 0x40U && rx[2] == 0x42U);
    CHECK(fw_i2c_mem_read(&i2c->dev, 0x3C, 0x10, 3U, rx, 1U, 10U) ==
          FW_EINVAL);
    CHECK(fw_i2c_mem_read(&i2c->dev, 0x3C, 0x100, 1U, rx, 1U, 10U) ==
          FW_EINVAL);
    CHECK(fw_i2c_mem_write(&i2c->dev, 0x3C, 0x1234, 2U, tx, 1U, 10U) == 1);
    CHECK(host.i2c_last_mem_size == 2U);

    /* 探测：默认有应答，注入 NODEV 后向上传递。 */
    CHECK(fw_i2c_probe(&i2c->dev, 0x3C, 10U) == FW_OK);
    CHECK(host.i2c_probes == 1U);
    host.i2c_probe_result = FW_ENODEV;
    CHECK(fw_i2c_probe(&i2c->dev, 0x3C, 10U) == FW_ENODEV);

    /* 驱动错误原样传播，并累计设备错误计数。 */
    {
        uint32_t errors = fw_dev_error_count(&i2c->dev);
        host.i2c_result = FW_EIO;
        CHECK(fw_i2c_transfer(&i2c->dev, 0x20, tx, 1U, NULL, 0U, 10U) ==
              FW_EIO);
        CHECK(fw_dev_error_count(&i2c->dev) == errors + 1U);
    }
    CHECK(fw_dev_error_count(NULL) == 0U);
}

/* ADC：通道读取、mV 换算、size/通道校验。 */
static void test_adc_class(void)
{
    fw_host_t host;
    fw_adc_t *adc;
    uint16_t raw = 0U;
    uint32_t mv = 0U;
    uint8_t buffer[4] = {0};
    fw_adc_config_t config = { 3300U, 12U, 84U };

    host_init(&host);
    host_register_devices(&host);
    adc = fw_adc_find("adc1");
    CHECK(adc != NULL);
    if (adc == NULL) return;

    host.adc_samples[8] = 2048U;
    host.adc_samples[9] = 4095U;

    CHECK(fw_adc_read(&adc->dev, 8, &raw, 10U) == FW_OK);
    CHECK(raw == 2048U);
    CHECK(fw_adc_read(&adc->dev, 9, &raw, 10U) == FW_OK);
    CHECK(raw == 4095U);

    CHECK(fw_adc_read_mv(&adc->dev, 8, &mv, 10U) == FW_OK);
    CHECK(mv == 1650U);
    CHECK(fw_adc_read_mv(&adc->dev, 9, &mv, 10U) == FW_OK);
    CHECK(mv == 3300U);

    /* 通用 read：off=channel，缓冲区至少 2 字节。 */
    CHECK(fw_dev_read(&adc->dev, 8, buffer, sizeof(buffer), 10U) == 2);
    CHECK(buffer[0] == (uint8_t)(2048U & 0xFFU));
    CHECK(fw_dev_read(&adc->dev, 8, buffer, 1U, 10U) == FW_EINVAL);
    CHECK(fw_adc_read(&adc->dev, 16, &raw, 10U) == FW_EINVAL);

    /* 配置。 */
    CHECK(fw_dev_ioctl(&adc->dev, 0, FW_CTL_ADC_SET_CONFIG, &config) ==
          (int)sizeof(config));
    CHECK(host.adc_config.sample_cycles == 84U);

    /* 驱动错误传播，并累计设备错误计数。 */
    {
        uint32_t errors = fw_dev_error_count(&adc->dev);
        host.adc_result = FW_EIO;
        CHECK(fw_adc_read(&adc->dev, 8, &raw, 10U) == FW_EIO);
        CHECK(fw_dev_error_count(&adc->dev) == errors + 1U);
    }
}

/* PWM：占空比、启停、运行期重配约束。 */
static void test_pwm_class(void)
{
    fw_host_t host;
    fw_pwm_t *pwm;
    fw_pwm_config_t config = { 1000U };
    fw_pwm_config_t readback = { 0U };

    host_init(&host);
    host_register_devices(&host);
    pwm = fw_pwm_find("pwm4");
    CHECK(pwm != NULL);
    if (pwm == NULL) return;

    CHECK(fw_pwm_set_duty(&pwm->dev, 0, 0U) == FW_OK);
    CHECK(fw_pwm_set_duty(&pwm->dev, 1, 500U) == FW_OK);
    CHECK(fw_pwm_set_duty(&pwm->dev, 2, 1000U) == FW_OK);
    CHECK(host.pwm_duty[0] == 0U && host.pwm_duty[1] == 500U);
    CHECK(host.pwm_duty[2] == 1000U);
    CHECK(fw_pwm_set_duty(&pwm->dev, 0, FW_PWM_DUTY_MAX + 1U) == FW_EINVAL);
    CHECK(fw_pwm_set_duty(&pwm->dev, FW_PWM_CHANNELS, 500U) == FW_EINVAL);

    /* 启动后拒绝重配；停止后可以改频率。 */
    CHECK(fw_pwm_start(&pwm->dev) == FW_OK);
    CHECK(host.pwm_started);
    CHECK(fw_dev_ioctl(&pwm->dev, 0, FW_CTL_PWM_SET_CONFIG, &config) ==
          FW_EBUSY);
    CHECK(fw_pwm_stop(&pwm->dev) == FW_OK);
    CHECK(!host.pwm_started);
    config.frequency_hz = 2000U;
    CHECK(fw_dev_ioctl(&pwm->dev, 0, FW_CTL_PWM_SET_CONFIG, &config) ==
          (int)sizeof(config));
    CHECK(fw_pwm_get_config(&pwm->dev, &readback) == FW_OK);
    CHECK(readback.frequency_hz == 2000U);

    /* ioctl SET_DUTY 路径。 */
    {
        uint32_t permille = 250U;
        CHECK(fw_dev_ioctl(&pwm->dev, 3, FW_CTL_PWM_SET_DUTY, &permille) ==
              FW_OK);
        CHECK(host.pwm_duty[3] == 250U);
    }

    /* 驱动错误传播，并累计设备错误计数。 */
    {
        uint32_t errors = fw_dev_error_count(&pwm->dev);
        host.pwm_result = FW_EIO;
        CHECK(fw_pwm_set_duty(&pwm->dev, 0, 100U) == FW_EIO);
        CHECK(fw_dev_error_count(&pwm->dev) == errors + 1U);
    }
}

void test_ext_peripherals_run(void)
{
    test_i2c_class();
    test_adc_class();
    test_pwm_class();
}
