# Board: STM32F407VE

## 板卡身份

| 项目 | 值 | 来源 |
|---|---|---|
| 名称 | `STM32F407VE` | `BOARD_NAME` |
| 版本 | `0.2.0` | `BOARD_VERSION` |
| Flash | 512 KB @ `0x08000000` | `BOARD_FLASH_*` |
| RAM | 128 KB @ `0x20000000` | `BOARD_RAM_*` |
| CCM | 64 KB @ `0x10000000`（不使用） | `BOARD_CCM_*` |
| 主栈/中断栈预留 | 4096 B | `BOARD_MIN_STACK_SIZE` |

启动横幅会打印名称、版本和上次复位原因；`board_name()/board_version()/
board_reset_cause()/board_reset_cause_name()` 供应用查询。

## 文件分布

| 文件 | 职责 |
|---|---|
| `Kconfig.board` | 时钟、板卡信息、内存、LED/控制台/UART/SPI/LCD 接线 |
| `board.h` | 板级能力 API；不暴露 HAL/驱动内部类型 |
| `board.c` | 薄组合根：platform boot → 设备注册 → 组件初始化 |
| `board_clock.c/h` | HSI/HSE 的 `stm32f407_clock_cfg_t` |
| `board_pins.c/h` | UART/SPI 描述符 + 端口/AF/SWD/重复引脚校验 |
| `board_devices.c` | pin/LED/serial/SPI/LCD 注册、控制台绑定 |
| `link.lds` | 包含生成的 `board_memory.ld` |

通用 HAL/NVIC/SysTick/console/panic 在 `src/platform/stm32f407/`，不在本目录。

## 默认接线

| 功能 | 信号 | 引脚 | 说明 |
|---|---|---|---|
| LED | — | PE2 | 高电平点亮；`BOARD_LED_ENABLE=y` |
| console/protocol | USART1 TX | PA9 | AF7，115200 8N1 |
| console/protocol | USART1 RX | PA10 | AF7 |
| UART2（默认关闭） | TX/RX | PD5/PD6 | AF7 |
| UART3（默认关闭） | TX/RX | PB10/PB11 | AF7 |
| SPI1 | SCK | PA5 | AF5 |
| SPI1 | MOSI | PA7 | AF5 |
| SPI1 | MISO | PA6 | AF5，`BOARD_SPI1_USE_MISO=n` |
| SPI2（默认关闭） | SCK/MOSI/MISO | PB13/PB15/PB14 | AF5，MISO 启用 |
| SPI3（默认关闭） | SCK/MOSI/MISO | PC10/PC12/PC11 | AF6 |
| LCD | CS | PA4 | 低有效 |
| LCD | DC | PA6 | 命令/数据 |
| LCD | RES | PA3 | 硬件复位 |
| LCD | BL | PA2 | 背光，高有效 |

LCD 为 ST7735S 128×160，X/Y offset=2/1，MADCTL=0xC0，默认不反相。

## 时钟

- 默认：HSI 16 MHz，SYSCLK=16 MHz，APB1/APB2 不分频，Flash latency=0。
- 可选：HSE 8 MHz → PLLM=8、PLLN=336、PLLP=2、PLLQ=7 → SYSCLK=168 MHz，
  AHB=/1、APB1=/4、APB2=/2，Flash latency=5。
- `board_clock.c` 负责把 Kconfig 选择转换成 `stm32f407_clock_cfg_t`；
  `stm32f407_clock.c` 只做 HAL 映射，不包含板级宏。
- 切换方式：`menuconfig` → `Board: STM32F407VE` → `System clock`。

## 初始化顺序

```text
rt_hw_board_init
  stm32f407_boot_init(board_clock_config())
    HAL_Init
    NVIC 分组 4
    时钟树（HSI/HSE）
    SysTick（HAL tick + rt_tick + fw_task_cpu_tick）
    延时钩子 / 可选 heap
    RCC 复位原因捕获
  board_devices_init()
    board_pins_validate()      端口 0..4、AF、SWD PA13/PA14、重复引脚
    drv_pin_register()         -> "pin"
    LED 引脚配置
    drv_serial_register_all()  -> "serial1..N"
    stm32f407_console_bind()   绑定控制台 serial
    drv_spi_register_all()     -> "spi1..N"
    LCD 引脚 + st7735s_init    -> "lcd"
  rt_components_board_init()（若启用）

main
  fw_dev_open(console/pin)
  board_serial_start_all()     arm 所有启用串口
  app_boot_display(board_lcd())
  app_tasks_init/start
  健康监督循环
```

## 引脚/AF 校验

`board_pins_validate()` 在设备注册前执行，检查：

- 端口 0=A .. 4=E、引脚 0..15；
- SWD PA13/PA14 不允许被占用；
- UART/SPI 的 AF 与引脚组合在板级 AF 表中；
- LED、LCD、UART、SPI 引脚互不重复；
- 已启用 SPI 的 MISO 引脚也参与校验。

Kconfig 仍可填入非法值，但启动会在 `board_devices_init` 返回错误后 panic；
`genconfig.py` 另外检查端口范围和 SWD 保留脚。

## 内存/链接

- `Kconfig.board` 的 `BOARD_FLASH_* / BOARD_RAM_* / BOARD_CCM_* /
  BOARD_MIN_STACK_SIZE` 是唯一来源。
- `genconfig.py` 生成 `profiles/<profile>/generated/board_memory.ld`；
  GCC `link.lds` 通过 `INCLUDE board_memory.ld` 使用。
- `gen_keil.py` 读取 `board_config.h` 的 FLASH/RAM 大小，生成
  `keil/stm32f407ve.sct`。
- Keil 启动文件 `keil/startup_stm32f407xx.s` 的 `Stack_Size` 目前为 4096 B，
  必须与 `BOARD_MIN_STACK_SIZE` 保持一致。

## 中断优先级

- SysTick 与 PendSV：最低（`0xFF`）。
- USART：`BOARD_UART_IRQ_PRIORITY`，默认 6。
- RT-Thread 临界区使用 PRIMASK；serial class 通过 driver 的 `lock/unlock` 保护 ring。
- USART ISR 在 `drv_serial.c` 内定义，只碰 ring/事件，不阻塞。
- SysTick 在 `stm32f407_irq.c`，先采样 CPU 再推进 tick。

## 调试

```text
arm-none-eabi-nm -n build/f407-debug/cmake/fw_v2.elf |
  grep -E "SysTick_Handler|rt_hw_console_output|PendSV_Handler|entry|stm32f407_panic"
```

`SysTick_Handler`/`rt_hw_console_output` 来自 platform，`PendSV_Handler` 来自 libcpu，
`entry` 来自 RT-Thread components.c。
