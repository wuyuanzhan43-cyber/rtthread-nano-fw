# Board: STM32F407VE

## 事实来源

板级接线只有一个手写事实源：

| 文件 | 内容 |
|---|---|
| `board_config.h` | 身份、内存/NVM、逻辑引脚、UART/SPI/I2C/ADC/PWM 通道、DMA 路由、LCD 面板参数 |
| `board.c` | 唯一的组合根：C 表 → 驱动描述符、设备注册、时钟选择、启动校验 |
| `board.h` | 对应用暴露的板级 API；不暴露 HAL/驱动内部类型 |
| `Kconfig.board` | 只保留构建策略：时钟源、启用项、波特率、IRQ、SPI 频率、I2C/ADC/PWM 参数、IWDG/fault |
| `link.lds` | GCC 内存区域（Flash 代码区、NVM、RAM、CCM、最小栈） |

`board_config.h` 是纯 C（只含 `stdbool.h`/`stdint.h` 和生成的
`board_policy.h`），因此 Host 测试可以直接包含并检查表结构。

Kconfig 生成到 `profiles/<profile>/generated/board_policy.h`，只包含
`BOARD_*` 策略宏；不再生成 `board_config.h`、`board_memory.ld`、
`board_keil.json` 或引脚/通道摘要。

## 默认接线

| 功能 | 信号 | 引脚 | 说明 |
|---|---|---|---|
| LED | — | PE2 | 高有效，`BOARD_LED_ENABLE=y` |
| console/protocol | USART1 TX/RX | PA9/PA10 | AF7，115200 8N1 |
| UART2（默认关闭） | TX/RX | PD5/PD6 | AF7 |
| UART3（默认关闭） | TX/RX | PB10/PB11 | AF7 |
| SPI1 | SCK/MISO/MOSI | PA5/PA6/PA7 | AF5；MISO 默认关闭 |
| SPI2（默认关闭） | SCK/MISO/MOSI | PB13/PB14/PB15 | AF5 |
| SPI3（默认关闭） | SCK/MISO/MOSI | PC10/PC11/PC12 | AF6 |
| I2C1 | SCL/SDA | PB6/PB7 | AF4，开漏，默认 100 kHz |
| I2C2（默认关闭） | SCL/SDA | PB10/PB11 | AF4；与 UART3 冲突 |
| I2C3（默认关闭） | SCL/SDA | PA8/PC9 | AF4 |
| ADC1 | IN8/IN9 | PB0/PB1 | 12 位，Vref 3.3V，单次轮询 |
| PWM4 | CH1..4 | PD12/PD13/PD14/PD15 | AF2，默认 1 kHz，初始化后不启动 |
| LCD | CS/DC/RES/BL | PA4/PA6/PA3/PA2 | ST7735S 128×160，offset=2/1，MADCTL=0xC0 |

默认 UART1 的 DMA 路由：RX=DMA2 Stream2 Channel4，TX=DMA2 Stream7 Channel4。
SPI1 只使用 TX DMA：DMA2 Stream3 Channel3；RX DMA 要求 MISO 已接线且
TX 路由存在。

## 修改板级接线

1. 只改 `board_config.h` 中的表/常量；
2. 策略项（时钟源、启用项、波特率、频率、IWDG）改 `Kconfig.board`，
   再执行 `python tools/fwconfig.py menuconfig --profile <name>` 或直接
   编辑 `profiles/<name>/.config`；
3. `python tools/fwconfig.py sync --all`；
4. `python tools/fwconfig.py gen-keil --profile f407ve`（如使用 Keil）；
5. 重新构建 CMake/Keil。

新增 UART/SPI 实例只需要在 C 表加行并在 `Kconfig.drivers` 打开
`FW_USING_UARTn`/`FW_USING_SPIn`；`board.c` 按 Kconfig 过滤后自动转换，
不需要新增 `.c` 文件。

## 初始化顺序

```text
rt_hw_board_init
  stm32f407_boot_init(&s_clock)
    HAL_Init / NVIC 分组 4 / 时钟树 / SysTick / 延时钩子 / 复位原因
  board_devices_init()
    stm32f407_fault_init()     安装 RT-Thread HardFault 异常钩子
    drv_dma_init()             清空 UART/SPI 共用的 DMA owner 表
    board_pins_validate()      端口/保留脚/重复引脚/DMA route 冲突
    drv_pin_register()         -> "pin"
    LED 引脚配置
    build_serial_hw()          C 表 -> drv_serial_hw_t，按 FW_USING_UARTn 过滤
    drv_serial_register_all()  -> "serial1..N"（按需 claim UART DMA）
    stm32f407_console_bind()   绑定控制台 serial
    build_spi_hw()             -> "spi1..N"（按需 claim SPI DMA）
    build_i2c_hw()             -> "i2c1..3"（AF_OD 主模式）
    build_adc_hw()             -> "adc1"（模拟引脚 + ADC1）
    build_pwm_hw()             -> "pwm4"（TIM4 四路，未启动）
    stm32f407_nvm_init()       参数存储 Flash 后端
    fw_param_init()            扫描 A/B 参数槽
    LCD 引脚 + st7735s_init    -> "lcd"
  rt_components_board_init()（若启用）

main
  fw_rtthread_log_bind()
  fw_dev_open(console/pin)
  board_serial_start_all()
  app_boot_display(board_lcd())
  app_tasks_init/start（rpc + worker）
  board_watchdog_start()（BOARD_IWDG_ENABLE）
  健康监督循环：无任务 LOST 时 board_watchdog_feed()
```

## 校验层级

| 阶段 | 位置 | 内容 |
|---|---|---|
| 编译期 | `board_config.h` | NVM 与代码区/Flash 总量关系、IRQ 优先级 `uart < dma < exti < 255`、HSE PLLM 范围 |
| Host 表测试 | `src/tests/host/test_board_config.c` | 端口/pin 范围、SWD 保留、UART TX≠RX、I2C SCL≠SDA、ADC/PWM 通道唯一、DMA route 范围、关键接线回归 |
| 静态一致性 | `tools/tests/test_static_board.py` | link.lds 与 board_config.h 内存常量、Keil IROM/IRAM/Stack_Size、profile 无旧生成物、board_config.h 不依赖 HAL/RTOS |
| 启动期 | `board.c: board_pins_validate()` | 端口/保留脚、重复引脚、AF 范围、DMA route 合法且 stream 不冲突、LCD SPI 实例有效 |
| 链接期 | `link.lds` | RAM 是否放得下最小栈 |

`check-boards` 已删除；板级接线不再有 manifest 校验。Host CTest 包含静态板
测试和两个 Keil `--check`。

## 中断优先级

- SysTick：255；UART：`BOARD_UART_IRQ_PRIORITY`（默认 6）；
- DMA：`BOARD_DMA_IRQ_PRIORITY`（默认 7）；
- SPI 错误中断：与 DMA 同优先级（7），只在 SPI DMA 启用时打开 NVIC；
- EXTI：`BOARD_EXTI_IRQ_PRIORITY`（默认 8）；
- 编译期校验 `uart < dma < exti < 255`。

## SPI DMA 接线限制

SPI1 的 MISO（PA6）与 LCD DC 共用同一引脚，因此：

- `BOARD_SPI1_USE_MISO=n` 时 SPI1 只允许 TX，DMA 也只分配 TX route；
- 打开 SPI1 MISO 会与 LCD DC 重复引脚，`board_pins_validate()` 返回
  `FW_EINVAL`，启动 panic；
- 全双工 DMA 请使用 SPI2/SPI3，或修改 `board_config.h` 的接线。

## I2C / ADC / PWM

- I2C1 使用 PB6/PB7（AF4），GPIO 固定为复用开漏；400 kHz 需要外部
  2.2k–4.7k 上拉。I2C2 的 PB10/PB11 与 UART3 冲突，两者不能同时启用。
- ADC1 使用 PB0/PB1（IN8/IN9），引脚配置为模拟输入；ADCCLK 按
  `PCLK2 / div <= 36 MHz` 自动选择分频。
- PWM4 使用 PD12..PD15（AF2）；初始化只配置通道，不启动输出。
  `HAL_TIM_PWM_Stop` 关闭整个定时器，因此 start/stop 是全局操作。
- 三者首版都不使用中断/DMA，也没有 RPC 命令。

## NVM / 参数存储

- 尾部 256KB 分成两个 128KB 扇区：
  - slot A：`0x08040000`
  - slot B：`0x08060000`
- 代码区因此是 256KB；`link.lds` 的 FLASH 区域、Keil IROM 和
  scatter 都由该常量派生。修改时同步 `BOARD_CODE_FLASH_SIZE_KB` /
  `BOARD_NVM_ORIGIN` / `BOARD_NVM_SIZE_KB`。
- `fw_param` 槽头 20 字节 + 数据，CRC32 校验，序号选择最新有效槽。
- 每次 save 擦除非活动槽；适合低频参数，不建议高频写入。
- 默认 `FW_PARAM_DATA_MAX=1024`，远小于 128KB 槽容量。

## 故障、看门狗与日志

- `stm32f407_fault.c`：HardFault 走 RT-Thread 异常钩子，MemManage/BusFault/
  UsageFault 有强符号入口；输出栈帧和 CFSR/HFSR/MMFAR/BFAR。
- `BOARD_FAULT_RESET=y` 时 fault/panic 后自动复位；默认停机便于调试器检查。
- `BOARD_IWDG_ENABLE=y` 时 main 只在 `fw_task_summary().lost == 0` 时喂狗；
  IWDG 启动后不可停止，调试时需要关闭。
- `FW_LOG_LEVEL` 控制 `fw_log` 的编译期上限；运行期可用
  `fw_log_set_level()` 进一步降低。
- 每个 `fw_dev_t` 保存 `error_count`，可由调试器/RPC 读取。

## 调试

```text
arm-none-eabi-nm -n build/f407-debug/cmake/fw_v2.elf |
  grep -E "SysTick_Handler|rt_hw_console_output|PendSV_Handler|entry|stm32f407_panic"
```

`SysTick_Handler`/`rt_hw_console_output` 来自 platform，`PendSV_Handler` 来自 libcpu，
`entry` 来自 RT-Thread components.c。
