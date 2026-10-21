/*
 * stm32f407_panic.h - STM32F407 致命错误现场。
 *
 * 层级：src/platform/stm32f407
 * 职责：关闭中断、保存文件/行号并原地停机，供调试器读取现场。
 * 上下文：启动早期、任务或中断上下文均可调用。
 * 依赖：标准 C。
 */
#ifndef STM32F407_PANIC_H
#define STM32F407_PANIC_H

/* 调试器可读取的 panic 现场。 */
extern volatile const char *g_stm32f407_panic_file;
extern volatile unsigned g_stm32f407_panic_line;

/*
 * 停机。
 *
 * 不复位芯片，避免掩盖故障；产品固件如需看门狗复位，
 * 应在此前喂狗或等待看门狗溢出。
 */
void stm32f407_panic(const char *file, unsigned line);

#endif
