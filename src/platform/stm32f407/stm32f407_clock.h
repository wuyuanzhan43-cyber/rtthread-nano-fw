/*
 * stm32f407_clock.h - STM32F407 数据驱动时钟树初始化。
 *
 * 层级：src/platform/stm32f407
 * 职责：按板级 clock_cfg 选择 HSI/HSE、PLL、AHB/APB 分频和 Flash latency。
 * 上下文：stm32f407_boot_init 中、调度器启动前调用。
 * 依赖：fw/status.h、stdbool.h、stdint.h。
 *
 * 设计说明：
 *   板级只需要提供频率/分频/等待周期等数据，不接触 HAL 类型；
 *   platform 负责把逻辑分频映射成 RCC_* 常量。
 */
#ifndef STM32F407_CLOCK_H
#define STM32F407_CLOCK_H

#include <stdbool.h>
#include <stdint.h>
#include "fw/status.h"

/*
 * 时钟配置。
 *
 * use_hse       - false: HSI 直接作为 SYSCLK；true: HSE + PLL；
 * hse_hz        - HSE 频率，仅 use_hse=true 时使用；
 * pll_m/n/p/q   - PLL 参数；p 是输出分频 2/4/6/8；
 * ahb_div       - 1/2/4/8/16/64/128/256/512；
 * apb1_div      - 1/2/4/8/16；
 * apb2_div      - 1/2/4/8/16；
 * flash_latency - 0..15，对应 FLASH_LATENCY_x。
 */
typedef struct {
    bool use_hse;
    uint32_t hse_hz;
    uint8_t pll_m;
    uint16_t pll_n;
    uint8_t pll_p;
    uint8_t pll_q;
    uint16_t ahb_div;
    uint8_t apb1_div;
    uint8_t apb2_div;
    uint8_t flash_latency;
} stm32f407_clock_cfg_t;

/*
 * 初始化系统时钟。
 *
 * 成功返回 FW_OK；配置非法返回 FW_EINVAL，HAL 配置失败返回 FW_EIO。
 * 调用前 HAL_Init() 已执行；调用后 SystemCoreClock 已更新。
 */
fw_status_t stm32f407_clock_init(const stm32f407_clock_cfg_t *cfg);

#endif
