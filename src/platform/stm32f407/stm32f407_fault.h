/*
 * stm32f407_fault.h - Cortex-M4 故障现场。
 *
 * 层级：src/platform/stm32f407
 * 职责：提供 HardFault/MemManage/BusFault/UsageFault 的强符号入口，
 *       把异常栈帧和 SCB 故障寄存器保存到全局变量并输出到控制台。
 * 上下文：异常上下文，禁止依赖调度器、堆和阻塞 API。
 */
#ifndef STM32F407_FAULT_H
#define STM32F407_FAULT_H

#include <stdint.h>

/* 调试器可直接读取的最近一次故障现场。 */
extern volatile uint32_t g_stm32f407_fault_frame[8];
extern volatile uint32_t g_stm32f407_fault_cfsr;
extern volatile uint32_t g_stm32f407_fault_hfsr;
extern volatile uint32_t g_stm32f407_fault_mmfar;
extern volatile uint32_t g_stm32f407_fault_bfar;
extern volatile uint32_t g_stm32f407_fault_exc_return;
extern volatile uint32_t g_stm32f407_fault_vector;

/*
 * 安装 RT-Thread 异常钩子。
 *
 * HardFault 由 RT-Thread 的 context_gcc.S/context_rvds.S 入口接管，
 * 它在打印前调用本钩子；钩子输出 CFSR/HFSR/MMFAR/BFAR 和栈帧后停机
 * 或复位。MemManage/BusFault/UsageFault 由本模块的强符号入口处理。
 */
void stm32f407_fault_init(void);

/*
 * 公共 C 处理体；由汇编入口或异常钩子以 (栈帧指针, EXC_RETURN) 调用。
 * 只做保存、格式化、直写控制台，然后按配置复位或停机。
 */
void stm32f407_fault_dump(uint32_t *frame, uint32_t exc_return);

#endif
