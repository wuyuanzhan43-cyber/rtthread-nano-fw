# Board: STM32F407VE

## 事实来源

板卡拓扑只在两处维护：

| 文件 | 内容 |
|---|---|
| `chips/stm32f407.json` | 封装/端口、内存、AF 矩阵、DMA 请求、EXTI 分组、HAL/Keil 器件信息 |
| `boards/stm32f407ve/board.json` | 板卡身份、时钟 variant、命名引脚、UART/SPI 接线、设备绑定 |
| `profiles/<profile>/profile.json` | 选择 board/chip/clock variant（`hsi16` 或 `hse_8_168`） |

`Kconfig.board` 只保留构建策略：启用项、波特率、IRQ 优先级、SPI 频率、最小栈。
引脚/AF/时钟/内存/控制台/LCD 面板参数不再从 Kconfig 读取。

生成后的人读摘要：

```text
profiles/<profile>/generated/board_summary.txt
```

引脚占用、DMA 路由、时钟和 manifest hash 都在这里；`BOARD.md` 不再手工维护事实表。

## 默认接线（来自 board.json）

| 功能 | 信号 | 引脚 | 说明 |
|---|---|---|---|
| LED | — | PE2 | 高有效，`BOARD_LED_ENABLE=y` |
| console/protocol | USART1 TX/RX | PA9/PA10 | AF7，115200 8N1 |
| UART2（默认关闭） | TX/RX | PD5/PD6 | AF7 |
| UART3（默认关闭） | TX/RX | PB10/PB11 | AF7 |
| SPI1 | SCK/MOSI/MISO | PA5/PA7/PA6 | AF5；MISO 默认关闭 |
| SPI2（默认关闭） | SCK/MOSI/MISO | PB13/PB15/PB14 | AF5 |
| SPI3（默认关闭） | SCK/MOSI/MISO | PC10/PC12/PC11 | AF6 |
| LCD | CS/DC/RES/BL | PA4/PA6/PA3/PA2 | ST7735S 128×160，offset=2/1，MADCTL=0xC0 |

## 文件分布

| 文件 | 职责 |
|---|---|
| `Kconfig.board` | 板级构建策略（启用、波特率、IRQ、SPI 频率、最小栈） |
| `board.h` | 板级能力 API；不暴露 HAL/驱动内部类型 |
| `board.c` | 薄组合根：platform boot → 设备注册 → 组件初始化 |
| `board_clock.c/h` | 把所选 clock variant 转成 `stm32f407_clock_cfg_t` |
| `board_pins.c/h` | UART/SPI 描述符与启动前校验 |
| `board_devices.c` | pin/LED/serial/SPI/LCD 注册、控制台绑定 |
| `link.lds` | 包含生成的 `board_memory.ld` |

通用 HAL/NVIC/SysTick/console/panic 在 `src/platform/stm32f407/`。

## 生成链

```text
profile.json + board.json + chip.json
  -> tools/boardgen.py 校验 + 生成
  -> generated/board_config.h    兼容 BOARD_* 宏
  -> generated/board_desc.h      身份/时钟/命名引脚/DMA 路由
  -> generated/board_memory.ld   代码 Flash（扣除 NVM）、RAM/CCM、最小栈
  -> generated/board_keil.json   Device/Cpu/FlashDll/IRAM/IROM/CLOCK
  -> generated/board_summary.txt 人工审查摘要
```

`gen_keil.py` 只消费 generated 文件，生成：

```text
keil/stm32f407ve.uvprojx      + keil/stm32f407ve.sct      (f407ve)
keil/stm32f407ve-hse.uvprojx  + keil/stm32f407ve-hse.sct  (f407ve-hse)
```

## 初始化顺序

```text
rt_hw_board_init
  stm32f407_boot_init(board_clock_config())
    HAL_Init / NVIC 分组 4 / 时钟树 / SysTick / 延时钩子 / 复位原因
  board_devices_init()
    board_pins_validate()      端口/AF/SWD/重复引脚
    drv_pin_register()         -> "pin"
    LED 引脚配置
    drv_serial_register_all()  -> "serial1..N"
    stm32f407_console_bind()   绑定控制台 serial
    drv_spi_register_all()     -> "spi1..N"
    LCD 引脚 + st7735s_init    -> "lcd"
  rt_components_board_init()（若启用）

main
  fw_dev_open(console/pin)
  board_serial_start_all()
  app_boot_display(board_lcd())
  app_tasks_init/start（rpc + worker）
  健康监督循环
```

## 校验层级

| 阶段 | 位置 | 内容 |
|---|---|---|
| manifest | `tools/boardgen.py` | schema、端口/保留脚、function+AF、重复引脚、控制台/LCD 启用、DMA 流冲突、时钟树、内存/NVM、IRQ 优先级 |
| 生成物 | `fwconfig.py check --all` / `check-boards` | 生成头、board_desc、memory、board_keil 是否最新 |
| 启动期 | `board_pins.c` | 运行时再检查一次端口/AF/SWD/重复引脚 |
| 链接期 | `link.lds` | RAM 是否放得下最小栈 |

`check-boards` 和 `gen-keil --check` 已纳入 Host CTest；Keil 启动文件 `Stack_Size`
由 `BOARD_MIN_STACK_SIZE` 自动生成，不再手工同步。

## 中断优先级

- SysTick：255；UART：`BOARD_UART_IRQ_PRIORITY`（默认 6）；
- DMA：`BOARD_DMA_IRQ_PRIORITY`（默认 7）；
- EXTI：`BOARD_EXTI_IRQ_PRIORITY`（默认 8）；
- 生成期校验 `uart < dma < exti < 255`。

## 调试

```text
arm-none-eabi-nm -n build/f407-debug/cmake/fw_v2.elf |
  grep -E "SysTick_Handler|rt_hw_console_output|PendSV_Handler|entry|stm32f407_panic"
```

`SysTick_Handler`/`rt_hw_console_output` 来自 platform，`PendSV_Handler` 来自 libcpu，
`entry` 来自 RT-Thread components.c。
