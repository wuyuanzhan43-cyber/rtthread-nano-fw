/*
 * board.c - STM32F407VE 板级组合根。
 *
 * 层级：src/platform/board/stm32f407ve
 * 职责：
 *   1. 调用 platform 启动（HAL/时钟/SysTick/heap/复位原因）；
 *   2. 注册板级设备；
 *   3. 执行 RT-Thread 组件初始化（如果启用）；
 *   4. 暴露板卡信息与复位原因。
 *
 * 上下文：rt_hw_board_init 在调度器启动前、main 线程创建之前执行。
 * 依赖：board.h、board_clock.h、board_config.h、
 *       stm32f407_boot.h、stm32f407_panic.h、rtthread.h。
 *
 * 启动顺序：
 *   stm32f407_boot_init(board_clock_config())
 *     -> board_devices_init()
 *        -> stm32f407_console_bind(...)
 *     -> rt_components_board_init()
 */
#include "board.h"
#include "board_clock.h"
#include "board_config.h"
#include "stm32f407_boot.h"
#include "stm32f407_panic.h"
#include <rtthread.h>

void rt_hw_board_init(void)
{
    if (stm32f407_boot_init(board_clock_config()) != FW_OK)
        stm32f407_panic(__FILE__, __LINE__);
    if (board_devices_init() != FW_OK)
        stm32f407_panic(__FILE__, __LINE__);

#ifdef RT_USING_COMPONENTS_INIT
    rt_components_board_init();
#endif
}

const char *board_name(void)
{
    return BOARD_NAME;
}

const char *board_version(void)
{
    return BOARD_VERSION;
}

uint32_t board_reset_cause(void)
{
    return stm32f407_reset_cause();
}

const char *board_reset_cause_name(uint32_t cause)
{
    return stm32f407_reset_cause_name(cause);
}
