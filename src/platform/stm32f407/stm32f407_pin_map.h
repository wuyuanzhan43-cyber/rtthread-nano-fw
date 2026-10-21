/*
 * stm32f407_pin_map.h - STM32F407VE 芯片级引脚/实例表。
 *
 * 层级：src/platform/stm32f407
 * 职责：
 *   1. 用宏表描述 GPIO 端口、引脚掩码和 EXTI 映射；
 *   2. 把 USART/SPI 实例编号映射到 HAL 符号；
 *   3. 作为 drv_pin.c 的编译期数据源，避免运行期 switch/表初始化。
 *
 * 上下文：全部是编译期常量，可在任何上下文使用。
 * 依赖：stm32f4xx_hal.h。
 *
 * 参考 mr-library 的地方：
 *   mr-library 用 `MR_PIN_PORT_CONFIG`、`MR_PIN_CONFIG` 等宏表把
 *   “端口数组 + 引脚数组 + IRQ 数组”放在板级头文件里，驱动只做
 *   下标查表。本项目沿用同样的组织方式，但省略了每个引脚的对象，
 *   只保留驱动需要的三张表。
 *
 * 端口编号约定（与 FW_PIN_NUMBER 一致）：
 *   0=A, 1=B, 2=C, 3=D, 4=E, 5=F, 6=G, 7=H, 8=I。
 *   STM32F407VE 为 100 脚封装，实际可用 GPIOA..GPIOE。
 */
#ifndef STM32F407_PIN_MAP_H
#define STM32F407_PIN_MAP_H

#include "stm32f4xx_hal.h"

/*
 * 端口指针表。
 *
 * 下标就是端口编号；drv_pin.c 用 (number >> 4) 取下标。
 * 表的大小决定了支持的最大端口数，当前 F407VE 只需要 A..E。
 */
#define DRV_PIN_PORT_CONFIG { GPIOA, GPIOB, GPIOC, GPIOD, GPIOE }

/*
 * 引脚掩码表。
 *
 * 下标是引脚号 0..15，值是 HAL 的 GPIO_PIN_x。
 * 因为 HAL 的掩码恰好是 1<<n，这张表看起来冗余，但保留它可以让
 * 驱动不依赖“HAL 宏一定是 1<<n”这个实现细节，也方便以后替换 HAL。
 */
#define DRV_PIN_CONFIG {                       \
    GPIO_PIN_0,  GPIO_PIN_1,  GPIO_PIN_2,  GPIO_PIN_3,  \
    GPIO_PIN_4,  GPIO_PIN_5,  GPIO_PIN_6,  GPIO_PIN_7,  \
    GPIO_PIN_8,  GPIO_PIN_9,  GPIO_PIN_10, GPIO_PIN_11, \
    GPIO_PIN_12, GPIO_PIN_13, GPIO_PIN_14, GPIO_PIN_15  \
}

/*
 * 引脚号到 EXTI 中断号的映射。
 *
 * STM32 的 EXTI 线按引脚号分组：
 *   EXTI0..4   每个引脚单独一条中断；
 *   EXTI5..9   共用 EXTI9_5_IRQn；
 *   EXTI10..15 共用 EXTI15_10_IRQn。
 *
 * 当前 pin 类还没有实现外部中断，这张表预留给后续扩展；
 * 保留它可以避免以后新增中断功能时再改板级头文件。
 */
#define DRV_PIN_IRQ_MAP_CONFIG {               \
    EXTI0_IRQn, EXTI1_IRQn, EXTI2_IRQn, EXTI3_IRQn, EXTI4_IRQn, \
    EXTI9_5_IRQn, EXTI9_5_IRQn, EXTI9_5_IRQn, EXTI9_5_IRQn, EXTI9_5_IRQn, \
    EXTI15_10_IRQn, EXTI15_10_IRQn, EXTI15_10_IRQn, EXTI15_10_IRQn, \
    EXTI15_10_IRQn, EXTI15_10_IRQn \
}

/*
 * USART 实例映射。
 *
 * Kconfig 用 1/2/3 表示 USART1/2/3，驱动通过 stm32f407_map.c 查表；
 * 这里保留宏名是为了让板级表更直观，实际解析仍集中在 map 模块。
 */
#define DRV_UART1_INSTANCE USART1
#define DRV_UART1_IRQ USART1_IRQn
#define DRV_UART2_INSTANCE USART2
#define DRV_UART2_IRQ USART2_IRQn
#define DRV_UART3_INSTANCE USART3
#define DRV_UART3_IRQ USART3_IRQn

/* SPI 实例映射；SPI 当前使用轮询传输，没有 IRQ 宏。 */
#define DRV_SPI1_INSTANCE SPI1
#define DRV_SPI2_INSTANCE SPI2
#define DRV_SPI3_INSTANCE SPI3

#endif
