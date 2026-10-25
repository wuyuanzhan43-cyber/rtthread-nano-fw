/*
 * board_config.h - STM32F407VE 板级唯一接线事实源（mr-library 风格）。
 *
 * 层级：src/platform/board/stm32f407ve
 *
 * 职责：
 *   1. 用纯 C 常量和表描述本板的引脚、外设通道、DMA 路由、
 *      LCD 面板参数、NVM 分区和内存布局；
 *   2. 作为“换板时只改一个文件”的入口：board.c 读取这些表并转换成
 *      drv_*_hw_t，不在其他地方重复写引脚/AF；
 *   3. 供 Host 测试直接包含（只依赖 C 标准头），在 PC 上校验表结构。
 *
 * 设计原则：
 *   1. 接线（物理事实）与策略（构建选择）分离：
 *        - 接线：引脚/AF/通道/DMA 路由/LCD 参数/NVM 分区/内存
 *          -> 本文件，换板或改硬件时修改；
 *        - 策略：时钟源、启用项、波特率、IRQ 优先级、SPI/I2C/ADC/PWM
 *          参数、IWDG/fault
 *          -> Kconfig.board，生成到 board_policy.h。
 *   2. 本文件是纯 C：只包含 <stdbool.h>、<stdint.h> 和生成的
 *      board_policy.h，不包含 HAL/RT-Thread/drv 头。这样 Host 测试
 *      可以在没有芯片头文件的环境里检查表，同时避免板级头把 HAL
 *      类型泄漏到上层。
 *   3. BOARD_* 内存/Keil 常量保持“一行式字面量”（如 0x08000000U、
 *      256U、"STM32F407VETx"）。tools/gen_keil.py 用正则读取这些值，
 *      不要写成带运算符的表达式，否则生成器无法解析。
 *   4. 表项的顺序不需要与实例号一致；board.c 用 instance 字段匹配，
 *      Kconfig 的 FW_USING_* 决定哪些表项真正注册。
 *
 * 数据流：
 *
 *   board_config.h (本文件，C 表)
 *        + board_policy.h (Kconfig 生成的策略)
 *             |
 *             v
 *   board.c:
 *     board_pins_validate()   结构校验（端口/重复/DMA 冲突）
 *     build_serial_hw()       UART 表 -> drv_serial_hw_t
 *     build_spi_hw()          SPI 表  -> drv_spi_hw_t
 *     build_i2c_hw()          I2C 表  -> drv_i2c_hw_t
 *     build_adc_hw()          ADC 表  -> drv_adc_hw_t
 *     build_pwm_hw()          PWM 表  -> drv_pwm_hw_t
 *             |
 *             v
 *   drv_*_register_all() -> fw_dev 注册表：
 *     "pin"、"serial1..3"、"spi1..3"、"i2c1..3"、"adc1"、"pwm4"、"lcd"
 *
 * 修改接线的标准流程：
 *   1. 改本文件对应的表/常量：
 *        - LED/LCD 控制脚：BOARD_PIN_TABLE_INIT、BOARD_LCD_INIT；
 *        - UART/SPI/I2C/ADC/PWM：对应的 *_TABLE_INIT；
 *        - DMA 路由：对应表项里的 dma_rx/dma_tx；
 *        - 内存/NVM：BOARD_*_ORIGIN/SIZE_KB（同时改 link.lds）；
 *   2. 策略项改 Kconfig.board 或 menuconfig；
 *   3. python tools/fwconfig.py sync --all
 *   4. python tools/fwconfig.py gen-keil --profile f407ve（如用 Keil）
 *   5. 重新构建 CMake/Keil。
 *
 *   启动期 board_pins_validate() 会拒绝端口越界、SWD 保留脚、
 *   重复引脚、非法 DMA 路由和 stream 冲突；Host 测试会检查表结构。
 *
 * 引脚编号约定：
 *   本文件用 {port, pin} 结构描述引脚；board.c 注册时转换成
 *   FW_PIN_NUMBER(port, pin) = (port << 4) | pin。
 *   port：0=A、1=B、2=C、3=D、4=E（F407VE 100 脚封装）。
 *   PA13/PA14 固定给 SWDIO/SWCLK，不能出现在任何板级表里。
 */
#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

#include <stdbool.h>
#include <stdint.h>
#include "board_policy.h"

/* ====================================================================== */
/* 1. 板卡身份                                                           */
/* ====================================================================== */

/*
 * 板卡名称/版本。
 *
 * BOARD_NAME 会出现在启动横幅、Keil 工程的 LayName 和调试信息里；
 * BOARD_VERSION 是板级描述版本，不等同于固件版本（FW_PROJECT_VERSION）。
 * 两者只能是字符串字面量，gen_keil.py 会解析 BOARD_NAME 生成 board_id。
 */
#define BOARD_NAME    "STM32F407VE"
#define BOARD_VERSION "0.2.0"

/* ====================================================================== */
/* 2. 内存布局与 NVM 分区                                                 */
/* ====================================================================== */

/*
 * STM32F407VE 的物理内存：
 *
 *   0x08000000 +----------------------------+
 *              | 代码区 256KB               |  FLASH 区域（link.lds / Keil IROM）
 *   0x08040000 +----------------------------+
 *              | NVM 参数区 256KB           |  stm32f407_nvm.c 使用，不参与链接
 *              |  slot A 128KB              |
 *              |  slot B 128KB              |
 *   0x08080000 +----------------------------+  总 Flash 512KB
 *
 *   0x10000000 +----------------------------+
 *              | CCM 64KB                   |  当前不使用（DMA 不能访问 CCM）
 *   0x10010000 +----------------------------+
 *
 *   0x20000000 +----------------------------+
 *              | 主 SRAM 128KB              |  .data/.bss/栈/RT-Thread 静态对象
 *   0x20020000 +----------------------------+
 *
 * 约束（编译期 / 静态测试都会检查）：
 *   CODE_FLASH_SIZE_KB + NVM_SIZE_KB == FLASH_SIZE_KB
 *   NVM_ORIGIN == FLASH_ORIGIN + CODE_FLASH_SIZE_KB * 1024
 *   NVM_SIZE_KB == 2 * NVM_SECTOR_KB（A/B 两个可独立擦除的扇区）
 *
 * 修改这些值时，必须同步：
 *   - link.lds 的 MEMORY 区域；
 *   - Keil 工程（运行 gen-keil 即可，IROM/IRAM/Stack 自动派生）；
 *   - tools/tests/test_static_board.py 会交叉校验。
 *
 * 注意：这些宏必须保持一行式字面量，gen_keil.py 才能解析。
 */
#define BOARD_FLASH_ORIGIN        0x08000000U /* Flash 起始地址 */
#define BOARD_FLASH_SIZE_KB       512U        /* 芯片总 Flash */
#define BOARD_CODE_FLASH_SIZE_KB  256U        /* 固件可用代码区 */
#define BOARD_NVM_ORIGIN          0x08040000U /* 参数区起始地址 */
#define BOARD_NVM_SIZE_KB         256U        /* 参数区总大小 */
#define BOARD_NVM_SECTOR_KB       128U        /* F407 尾部扇区大小 */
#define BOARD_RAM_ORIGIN          0x20000000U /* 主 SRAM 起始地址 */
#define BOARD_RAM_SIZE_KB         128U        /* 主 SRAM 大小 */
#define BOARD_CCM_ORIGIN          0x10000000U /* CCM 起始地址 */
#define BOARD_CCM_SIZE_KB         64U         /* CCM 大小（当前不链接） */

/*
 * 主栈/中断栈预留（字节）。
 *
 * link.lds 的 _Min_Stack_Size 和 Keil 启动文件的 Stack_Size 都使用
 * 这个值；gen_keil.py 会自动同步 Keil 启动文件。改这里后要重新
 * 生成 Keil 工程并检查 RAM 是否仍放得下。
 */
#define BOARD_MIN_STACK_SIZE      4096U

/*
 * Keil AC5 工程字段。
 *
 * gen_keil.py 读取这三个字符串生成 uvprojx 的 Device/Vendor/
 * FlashDriverDll。FLASH_ALGO 只是 Keil 的算法名，实际下载范围由
 * CODE_FLASH_SIZE_KB 决定（不会擦到 NVM）。
 */
#define BOARD_KEIL_DEVICE         "STM32F407VETx"
#define BOARD_KEIL_VENDOR         "STMicroelectronics"
#define BOARD_KEIL_FLASH_ALGO     "STM32F4xx_512"

/* ====================================================================== */
/* 3. 板级固定参数（不属于 Kconfig 策略）                                 */
/* ====================================================================== */

/*
 * 外部晶振频率。
 *
 * HSI profile 不使用 HSE，但 HAL 仍要求 HSE_VALUE 有定义；
 * HSE profile 用它计算 PLLM = HSE / 1MHz，并验证 2..63 的范围。
 */
#define BOARD_HSE_VALUE           8000000U

/*
 * 控制台串口实例号（1..3）。
 *
 * board.c 在串口注册完成后用 drv_serial_instance(BOARD_CONSOLE_UART)
 * 找到对象并绑定到 RT-Thread 的 rt_hw_console_output；这个实例必须在
 * UART 表中存在，且对应 profile 打开了 FW_USING_UARTn。
 */
#define BOARD_CONSOLE_UART        1U

/*
 * 板载 LED 有效电平。
 *
 * 1 = 高电平点亮（GPIO 输出 1 亮）；
 * 0 = 低电平点亮（常见于 LED 接 VCC 的板子）。
 * board.c 用它决定初始化时写 0 还是 1 让 LED 熄灭。
 */
#define BOARD_LED_ACTIVE_HIGH     1

/* ====================================================================== */
/* 4. NVM 后端别名                                                       */
/* ====================================================================== */

/*
 * stm32f407_nvm.c 通过下面这组宏拿到参数区描述：
 *
 *   BOARD_NVM_ENABLED  - 0 时后端不初始化，fw_param_init 也不会调用；
 *   BOARD_NVM_BASE     - fw_param 后端读写的起始地址；
 *   BOARD_NVM_SLOT_KB  - 单个 A/B 槽大小 = 总大小 / 2。
 *
 * 这里刻意保留 NVM_ORIGIN / NVM_SIZE_KB / NVM_SECTOR_KB 作为
 * 板级“事实”名字，再用别名映射到驱动约定的名字，避免两个模块
 * 各自维护一套命名。BOARD_NVM_SECTOR_KB_ 是兼容别名。
 */
#define BOARD_NVM_ENABLED         1
#define BOARD_NVM_BASE            BOARD_NVM_ORIGIN
#define BOARD_NVM_SLOT_KB         (BOARD_NVM_SIZE_KB / 2U)
#define BOARD_NVM_SECTOR_KB_      BOARD_NVM_SECTOR_KB

/*
 * 编译期内存关系检查。
 *
 * 如果代码区 + NVM 不等于总 Flash，或者 NVM 没有紧接代码区，这里会
 * 产生负数组长度，编译直接失败。比运行时 panic 更早暴露配置错误。
 *
 * 注意：这个检查只覆盖本文件自己的常量；link.lds 和 Keil 的一致性
 * 由 tools/tests/test_static_board.py 交叉校验。
 */
typedef char board_nvm_tail_check[
    (BOARD_CODE_FLASH_SIZE_KB + BOARD_NVM_SIZE_KB == BOARD_FLASH_SIZE_KB &&
     BOARD_NVM_ORIGIN ==
         BOARD_FLASH_ORIGIN + BOARD_CODE_FLASH_SIZE_KB * 1024U) ? 1 : -1];

/* ====================================================================== */
/* 5. 策略默认值（由 board_policy.h 覆盖）                                */
/* ====================================================================== */

/*
 * Kconfig 生成的 board_policy.h 会先定义这些宏；本文件的 #ifndef 只在
 * 宏缺失时提供安全默认值。
 *
 * 两类使用场景：
 *   1. 固件构建：generated/ 在 include 路径最前，board_policy.h 提供
 *      profile 的真实策略；
 *   2. Host 测试：host profile 的 board_policy.h 是空头，于是这里用
 *      默认值描述“一块功能完整的目标板”，让板级表测试不依赖某个
 *      profile 的启用项。
 *
 * 注意：不能在 Host 的 board_policy.h 里把 BOARD_CLOCK_* 等定义成 0，
 * 否则这里的 #ifndef 不会生效，Host 测试会看到一块“全部关闭”的板子。
 */

/* LED/LCD 是否启用（board.c 用它们决定是否注册/配置对应设备）。 */
#ifndef BOARD_LED_ENABLE
#define BOARD_LED_ENABLE 1
#endif
#ifndef BOARD_LCD_ENABLE
#define BOARD_LCD_ENABLE 1
#endif

/* 所有 UART 共用的默认波特率与 NVIC 抢占优先级。 */
#ifndef BOARD_UART_BAUD
#define BOARD_UART_BAUD 115200U
#endif
#ifndef BOARD_UART_IRQ_PRIORITY
#define BOARD_UART_IRQ_PRIORITY 6U
#endif

/* DMA stream 中断优先级（UART/SPI DMA 共用 drv_dma 的 NVIC 配置）。 */
#ifndef BOARD_DMA_IRQ_PRIORITY
#define BOARD_DMA_IRQ_PRIORITY 7U
#endif

/* pin EXTI 中断优先级（FW_USING_PIN_EXTI 时使用）。 */
#ifndef BOARD_EXTI_IRQ_PRIORITY
#define BOARD_EXTI_IRQ_PRIORITY 8U
#endif

/*
 * SPI 策略：
 *   USE_MISO - 是否使用 MISO。SPI1 的 MISO(PA6) 与 LCD DC 共用，
 *              默认关闭，因此 SPI1 只能单线发送；SPI2/SPI3 默认全双工。
 *   MAX_HZ   - 驱动计算 SCK 分频时的上限，不是实际波形保证。
 */
#ifndef BOARD_SPI1_USE_MISO
#define BOARD_SPI1_USE_MISO 0
#endif
#ifndef BOARD_SPI1_MAX_HZ
#define BOARD_SPI1_MAX_HZ 12000000U
#endif
#ifndef BOARD_SPI2_USE_MISO
#define BOARD_SPI2_USE_MISO 1
#endif
#ifndef BOARD_SPI2_MAX_HZ
#define BOARD_SPI2_MAX_HZ 12000000U
#endif
#ifndef BOARD_SPI3_USE_MISO
#define BOARD_SPI3_USE_MISO 1
#endif
#ifndef BOARD_SPI3_MAX_HZ
#define BOARD_SPI3_MAX_HZ 12000000U
#endif

/* I2C 总线频率（标准模式 100 kHz；快速模式 400 kHz 需要外部上拉）。 */
#ifndef BOARD_I2C1_SPEED_HZ
#define BOARD_I2C1_SPEED_HZ 100000U
#endif
#ifndef BOARD_I2C2_SPEED_HZ
#define BOARD_I2C2_SPEED_HZ 100000U
#endif
#ifndef BOARD_I2C3_SPEED_HZ
#define BOARD_I2C3_SPEED_HZ 100000U
#endif

/*
 * ADC1 电气参数：
 *   VREF_MV         - 参考电压（mV），用于把原始码换算成 mV；
 *   RESOLUTION_BITS - 8/10/12 位；
 *   SAMPLE_CYCLES   - 采样周期，必须是 3/15/28/56/84/112/144/480。
 *
 * 通道与引脚在下面的 BOARD_ADC_TABLE_INIT 中。
 */
#ifndef BOARD_ADC1_VREF_MV
#define BOARD_ADC1_VREF_MV 3300U
#endif
#ifndef BOARD_ADC1_RESOLUTION_BITS
#define BOARD_ADC1_RESOLUTION_BITS 12U
#endif
#ifndef BOARD_ADC1_SAMPLE_CYCLES
#define BOARD_ADC1_SAMPLE_CYCLES 84U
#endif

/* TIM4 PWM 的初始频率；通道与引脚在 BOARD_PWM_TABLE_INIT 中。 */
#ifndef BOARD_PWM4_PERIOD_HZ
#define BOARD_PWM4_PERIOD_HZ 1000U
#endif

/* 独立看门狗：启动后不可停止，默认关闭，f407ve-dma 用于验证。 */
#ifndef BOARD_IWDG_ENABLE
#define BOARD_IWDG_ENABLE 0
#endif
#ifndef BOARD_IWDG_TIMEOUT_MS
#define BOARD_IWDG_TIMEOUT_MS 5000U
#endif

/* fault/panic 输出后是否自动复位；默认停机，便于调试器检查现场。 */
#ifndef BOARD_FAULT_RESET
#define BOARD_FAULT_RESET 0
#endif

/*
 * 时钟源选择（Kconfig 是二选一 choice）。
 *
 * board.c 用 #if BOARD_CLOCK_HSE_8_168 选择时钟树：
 *   HSI 16MHz：SYSCLK=16MHz，APB 不分频，Flash latency=0；
 *   HSE 8MHz ：PLL 8MHz -> 168MHz，APB1=/4、APB2=/2，latency=5。
 *
 * 下面的默认值只用于没有 board_policy.h 的极端情况（例如手工写
 * 一个临时 Host 测试）；正常构建一定由 board_policy.h 覆盖。
 */
#ifndef BOARD_CLOCK_HSI_16
#define BOARD_CLOCK_HSI_16 1
#endif
#ifndef BOARD_CLOCK_HSE_8_168
#define BOARD_CLOCK_HSE_8_168 0
#endif

/*
 * IRQ 优先级约束。
 *
 * 数字越小优先级越高。约定 UART < DMA < EXTI，保证串口中断可以
 * 抢占 DMA 完成回调，而 EXTI 不会打断前两者。255 是 RT-Thread
 * SysTick 使用的级别，板级中断不能占用。
 */
#if BOARD_UART_IRQ_PRIORITY >= BOARD_DMA_IRQ_PRIORITY || \
    BOARD_DMA_IRQ_PRIORITY >= BOARD_EXTI_IRQ_PRIORITY || \
    BOARD_EXTI_IRQ_PRIORITY >= 255
#error "IRQ priority must satisfy uart < dma < exti < 255"
#endif

/* ====================================================================== */
/* 6. 基础数据类型                                                       */
/* ====================================================================== */

/*
 * 一个物理 GPIO。
 *
 * port：0=A、1=B、2=C、3=D、4=E；
 * pin ：0..15。
 *
 * 用结构体而不是一个压缩整数，是为了让表在源码里可读、可搜索；
 * board.c 注册时再转成 FW_PIN_NUMBER 编码。
 */
typedef struct {
    uint8_t port; /* 端口索引：0=A..4=E */
    uint8_t pin;  /* 引脚号：0..15 */
} board_pin_t;

/*
 * 一条 DMA 路由。
 *
 * controller：0=未分配，1=DMA1，2=DMA2；
 * stream    ：0..7；
 * channel   ：0..7（F4 的 DMA 请求按 channel 选择）。
 *
 * “controller=0”是本文件的哨兵值，board.c 转换时会变成驱动里的
 * DRV_DMA_NONE(0xFF)。驱动只会在 enabled=true 时使用路由，因此
 * 未分配的 0/0/0 不会被误当成 DMA1 Stream0 Channel0。
 */
typedef struct {
    uint8_t controller; /* 0=none, 1=DMA1, 2=DMA2 */
    uint8_t stream;     /* 0..7 */
    uint8_t channel;    /* 0..7 */
} board_dma_route_t;

/*
 * 板级命名引脚 ID。
 *
 * 这些引脚不是外设复用脚，而是普通 GPIO 控制脚（LED、LCD 控制）。
 * 用枚举而不是直接把工程塞进各设备配置，方便 board_config.h 集中
 * 描述、board.c 统一转换成 FW_PIN_NUMBER。
 *
 * BOARD_PIN_COUNT 同时作为 s_pins[] 的数组长度和 board_pins_validate()
 * 的遍历上限；新增命名引脚时要在枚举和 BOARD_PIN_TABLE_INIT 同时加。
 */
typedef enum {
    BOARD_PIN_LED = 0,  /* 板载 LED */
    BOARD_PIN_LCD_CS,   /* ST7735S 片选（低有效） */
    BOARD_PIN_LCD_DC,   /* ST7735S 数据/命令选择（高=数据） */
    BOARD_PIN_LCD_RES,  /* ST7735S 硬件复位（低有效） */
    BOARD_PIN_LCD_BL,   /* LCD 背光控制 */
    BOARD_PIN_COUNT     /* 哨兵：命名引脚数量 */
} board_pin_id_t;

/*
 * 命名引脚表。
 *
 * 使用指定初始化器，保证下标与 board_pin_id_t 一一对应；新增/删除
 * 枚举项时即使忘记调整顺序，也不会把 LED 的引脚错配给 LCD。
 *
 * 当前板：
 *   LED     = PE2
 *   LCD_CS  = PA4
 *   LCD_DC  = PA6（与 SPI1 MISO 共用，因此 SPI1 默认关闭 MISO）
 *   LCD_RES = PA3
 *   LCD_BL  = PA2
 */
#define BOARD_PIN_TABLE_INIT                    \
{                                               \
    [BOARD_PIN_LED]     = { 4U, 2U }, /* PE2 */  \
    [BOARD_PIN_LCD_CS]  = { 0U, 4U }, /* PA4 */  \
    [BOARD_PIN_LCD_DC]  = { 0U, 6U }, /* PA6 */  \
    [BOARD_PIN_LCD_RES] = { 0U, 3U }, /* PA3 */  \
    [BOARD_PIN_LCD_BL]  = { 0U, 2U }, /* PA2 */  \
}

/* ====================================================================== */
/* 7. UART 通道表                                                        */
/* ====================================================================== */

/*
 * 一个 UART 实例的完整接线。
 *
 * instance - USART 编号（1..3），同时也是驱动槽位/设备名 serialN 的编号；
 * tx/rx    - 发送/接收引脚；
 * af       - 复用功能号（USART1..3 通常为 AF7）；
 * dma_rx   - RX DMA 路由（controller=0 表示没有分配）；
 * dma_tx   - TX DMA 路由。
 *
 * 是否真的启用某个实例、是否真的使用 DMA，由以下两件事共同决定：
 *   1. Kconfig：FW_USING_UARTn、FW_USING_UARTn_DMA_RX/TX；
 *   2. 本表：route.controller != 0。
 *
 * 也就是说，表里可以提前写好一条可用路由，但 profile 没打开 DMA 时
 * board.c 会把 dma_*_enabled 置 false，驱动继续走 RXNE/TXE 中断。
 *
 * 当前接线：
 *   USART1：TX=PA9  RX=PA10  AF7  RX=DMA2S2CH4  TX=DMA2S7CH4
 *   USART2：TX=PD5  RX=PD6   AF7  （无 DMA）
 *   USART3：TX=PB10 RX=PB11  AF7  （无 DMA）
 *
 * 注意：
 *   - TX 与 RX 不能是同一引脚；
 *   - UART2 的 PD5/PD6 与 I2C2 的 PB10/PB11 不冲突，但 I2C2 与
 *     UART3 共用 PB10/PB11，两者不能同时启用（启动校验会拒绝）；
 *   - DMA 路由必须与 STM32F407 数据手册的 DMA 请求表一致，否则
 *     传输会静默失败；Host 测试只检查结构，电气正确性需上板验证。
 */
typedef struct {
    uint8_t instance;      /* USART 编号 1..3 */
    board_pin_t tx;        /* TX 引脚 */
    board_pin_t rx;        /* RX 引脚 */
    uint8_t af;            /* 复用功能号 */
    board_dma_route_t dma_rx; /* RX DMA 路由；controller=0 表示无 */
    board_dma_route_t dma_tx; /* TX DMA 路由；controller=0 表示无 */
} board_uart_t;

#define BOARD_UART_TABLE_INIT                                         \
{                                                                     \
    /* USART1：控制台/协议主串口，PA9/PA10，AF7，RX/TX 都有 DMA 路由 */ \
    { 1U, {0U, 9U},  {0U, 10U}, 7U, {2U, 2U, 4U}, {2U, 7U, 4U} },     \
    /* USART2：PD5/PD6，AF7，默认未启用 */                             \
    { 2U, {3U, 5U},  {3U, 6U},  7U, {1U, 5U, 4U}, {1U, 6U, 4U} },     \
    /* USART3：PB10/PB11，AF7，默认未启用；与 I2C2 共用引脚 */          \
    { 3U, {1U, 10U}, {1U, 11U}, 7U, {1U, 1U, 4U}, {1U, 3U, 4U} },     \
}

/* ====================================================================== */
/* 8. SPI 通道表                                                         */
/* ====================================================================== */

/*
 * 一个 SPI 实例的完整接线。
 *
 * instance - SPI 编号（1..3），对应设备名 spiN；
 * sck/mosi/miso - 三根信号线（MISO 在 USE_MISO=0 时不配置）；
 * af       - 复用功能号（SPI1/SPI2=AF5，SPI3=AF6）；
 * dma_rx   - RX DMA 路由；
 * dma_tx   - TX DMA 路由。
 *
 * RX DMA 的前提（board.c/驱动都会检查）：
 *   1. BOARD_SPIn_USE_MISO=1；
 *   2. TX 路由存在（HAL 在 2LINES master 下接收需要 hdmatx）；
 *   3. RX 路由 controller != 0。
 *
 * 当前接线：
 *   SPI1：SCK=PA5 MISO=PA6 MOSI=PA7 AF5，仅 TX DMA=DMA2S3CH3
 *         （MISO 与 LCD DC 冲突，所以默认 USE_MISO=0）
 *   SPI2：SCK=PB13 MISO=PB14 MOSI=PB15 AF5（默认未启用）
 *   SPI3：SCK=PC10 MISO=PC11 MOSI=PC12 AF6（默认未启用）
 */
typedef struct {
    uint8_t instance;      /* SPI 编号 1..3 */
    board_pin_t sck;       /* 时钟线 */
    board_pin_t mosi;      /* 主出从入 */
    board_pin_t miso;      /* 主入从出 */
    uint8_t af;            /* 复用功能号 */
    board_dma_route_t dma_rx; /* RX DMA 路由；controller=0 表示无 */
    board_dma_route_t dma_tx; /* TX DMA 路由 */
} board_spi_t;

#define BOARD_SPI_TABLE_INIT                                          \
{                                                                     \
    /* SPI1：LCD 使用，PA5/PA6/PA7，AF5；默认 MISO 关闭，只有 TX DMA */  \
    { 1U, {0U, 5U},  {0U, 7U},  {0U, 6U},  5U,                        \
      {2U, 0U, 3U}, {2U, 3U, 3U} },                                   \
    /* SPI2：PB13/PB14/PB15，AF5；默认未启用 */                         \
    { 2U, {1U, 13U}, {1U, 15U}, {1U, 14U}, 5U,                        \
      {1U, 3U, 0U}, {1U, 4U, 0U} },                                   \
    /* SPI3：PC10/PC11/PC12，AF6；默认未启用 */                         \
    { 3U, {2U, 10U}, {2U, 12U}, {2U, 11U}, 6U,                        \
      {1U, 0U, 0U}, {1U, 5U, 0U} },                                   \
}

/* ====================================================================== */
/* 9. I2C 通道表                                                         */
/* ====================================================================== */

/*
 * 一个 I2C 实例的接线。
 *
 * instance - I2C 编号（1..3）；
 * scl/sda  - 时钟/数据线（GPIO 固定为 AF_OD 开漏）；
 * af       - 复用功能号，I2C1..3 均为 AF4。
 *
 * 当前接线：
 *   I2C1：PB6/PB7（默认启用，100 kHz）
 *   I2C2：PB10/PB11（默认关闭；与 UART3 冲突）
 *   I2C3：PA8/PC9（默认关闭）
 *
 * 400 kHz 需要外部上拉；内部上拉只适合低速短距离。
 */
typedef struct {
    uint8_t instance; /* I2C 编号 1..3 */
    board_pin_t scl;  /* 时钟线 */
    board_pin_t sda;  /* 数据线 */
    uint8_t af;       /* 复用功能号，固定 AF4 */
} board_i2c_t;

#define BOARD_I2C_TABLE_INIT                     \
{                                               \
    /* I2C1：PB6/PB7，AF4 */                     \
    { 1U, {1U, 6U},  {1U, 7U},  4U },            \
    /* I2C2：PB10/PB11，AF4；与 UART3 冲突 */     \
    { 2U, {1U, 10U}, {1U, 11U}, 4U },            \
    /* I2C3：PA8/PC9，AF4 */                     \
    { 3U, {0U, 8U},  {2U, 9U},  4U },            \
}

/* ====================================================================== */
/* 10. ADC 通道表                                                        */
/* ====================================================================== */

/*
 * 一个 ADC 通道。
 *
 * channel - ADC 通道号 0..15；
 * pin     - 对应的模拟输入引脚（没有 AF，配置为模拟模式）。
 *
 * 当前接线：
 *   ADC1_IN8 = PB0
 *   ADC1_IN9 = PB1
 *
 * 驱动首版只支持单通道、单次转换、软件触发；channel 必须与实际
 * 引脚匹配（F407 的 ADC1_INn 引脚是固定的），否则采样值没有意义。
 */
typedef struct {
    uint8_t channel;   /* ADC 通道 0..15 */
    board_pin_t pin;   /* 模拟输入引脚 */
} board_adc_channel_t;

/*
 * 一个 ADC 实例。
 *
 * instance - ADC 编号（当前只支持 1）；
 * count    - 本实例已接线的通道数，最大 4；
 * channels - 通道表；未使用的项保持 0。
 *
 * 是否启用由 Kconfig 的 FW_USING_ADC1 决定；电气参数
 * （Vref/分辨率/采样周期）来自上面的策略宏。
 */
typedef struct {
    uint8_t instance; /* ADC 编号 */
    uint8_t count;    /* 已接线通道数 */
    board_adc_channel_t channels[4]; /* 通道 -> 引脚 */
} board_adc_t;

#define BOARD_ADC_TABLE_INIT                                      \
{                                                                 \
    /* ADC1：IN8=PB0、IN9=PB1 */                                   \
    { 1U, 2U, { {8U, {1U, 0U}}, {9U, {1U, 1U}} } },                \
}

/* ====================================================================== */
/* 11. PWM 通道表                                                        */
/* ====================================================================== */

/*
 * 一个 PWM 通道。
 *
 * channel - 定时器通道号 1..4；
 * pin     - 输出引脚；
 * af      - 复用功能号（TIM4 为 AF2）。
 *
 * 当前接线：
 *   TIM4_CH1 = PD12
 *   TIM4_CH2 = PD13
 *   TIM4_CH3 = PD14
 *   TIM4_CH4 = PD15
 *
 * 驱动首版只支持 TIM4，且 start/stop 作用于整个定时器。
 */
typedef struct {
    uint8_t channel; /* 定时器通道 1..4 */
    board_pin_t pin; /* 输出引脚 */
    uint8_t af;      /* 复用功能号 */
} board_pwm_channel_t;

/*
 * 一个 PWM 实例。
 *
 * instance - 定时器编号（当前固定 4）；
 * count    - 已接线通道数，最大 4；
 * channels - 通道表。
 *
 * 频率来自策略宏 BOARD_PWM4_PERIOD_HZ；初始化只配置通道，不启动输出。
 */
typedef struct {
    uint8_t instance; /* 定时器编号 */
    uint8_t count;    /* 已接线通道数 */
    board_pwm_channel_t channels[4]; /* 通道 -> 引脚/AF */
} board_pwm_t;

#define BOARD_PWM_TABLE_INIT                                      \
{                                                                 \
    /* TIM4：CH1..CH4 = PD12/PD13/PD14/PD15，AF2 */                \
    { 4U, 4U, {                                                   \
        {1U, {3U, 12U}, 2U}, {2U, {3U, 13U}, 2U},                 \
        {3U, {3U, 14U}, 2U}, {4U, {3U, 15U}, 2U} } },             \
}

/* ====================================================================== */
/* 12. LCD 面板                                                          */
/* ====================================================================== */

/*
 * ST7735S 面板的板级描述。
 *
 * spi_instance  - 使用哪个 SPI 设备（1..3，必须与 SPI 表中的实例一致
 *                 且该 profile 打开 FW_USING_SPIn）；
 * cs/dc/res/bl  - 四个控制脚，引用 board_pin_id_t，实际引脚在
 *                 BOARD_PIN_TABLE_INIT 中；
 * width/height  - 面板逻辑分辨率（像素）；
 * x_offset/y_offset - 显示窗口偏移。ST7735S 的 128x160 面板常见
 *                 偏移为 (2,1) 或 (0,0)，取决于玻璃批次；
 * madctl        - MADCTL 寄存器值，控制行列扫描方向和 RGB/BGR；
 *                 0xC0 = MX|MY（旋转 180°），当前启动画面按这个方向写；
 * invert        - 是否发送 INVON（部分批次需要反色）；
 * bl_active_high- 背光有效电平。
 *
 * 注意：LCD DC(PA6) 与 SPI1 MISO 冲突。SPI1 默认 USE_MISO=0 时
 * 可以正常使用；如果强行打开 SPI1 MISO，board_pins_validate() 会
 * 因重复引脚拒绝启动。
 */
typedef struct {
    uint8_t spi_instance;  /* 使用的 SPI 实例 1..3 */
    board_pin_id_t cs;     /* 片选控制脚 */
    board_pin_id_t dc;     /* 数据/命令选择 */
    board_pin_id_t res;    /* 硬件复位 */
    board_pin_id_t bl;     /* 背光控制 */
    uint16_t width;        /* 逻辑宽度（像素） */
    uint16_t height;       /* 逻辑高度（像素） */
    uint16_t x_offset;     /* 列偏移 */
    uint16_t y_offset;     /* 行偏移 */
    uint8_t madctl;        /* MADCTL 寄存器值 */
    bool invert;           /* 是否需要反色 */
    bool bl_active_high;   /* 背光是否高有效 */
} board_lcd_t;

/*
 * 当前 LCD：SPI1 + CS/DC/RES/BL 命名引脚，128x160，偏移 (2,1)，
 * MADCTL=0xC0，不反色，背光高有效。
 */
#define BOARD_LCD_INIT                                            \
{ 1U, BOARD_PIN_LCD_CS, BOARD_PIN_LCD_DC, BOARD_PIN_LCD_RES,      \
  BOARD_PIN_LCD_BL, 128U, 160U, 2U, 1U, 0xC0U, false, true }

#endif /* BOARD_CONFIG_H */
