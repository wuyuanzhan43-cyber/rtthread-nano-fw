# Embedded Framework v2 · RT-Thread Nano 3.1.5

独立工程，不依赖旧版目录或外部 checkout。

```text
src/app（main 组合根 + rpc 业务 + boot 启动画面 + assets）
        │
src/platform/board/stm32f407ve（board.c 组合根 + board_config.h 接线表 + link.lds）
        │
src/platform/stm32f407（pin/serial/spi 驱动、映射、HAL 配置）
        │
src/framework（mr 风格设备核心 + 设备类 + 协议 + 面板驱动 + 任务管理 + RT-Thread 端口）
        │
RT-Thread Nano（调度/IPC/时间基）
```

离线文档：

- 框架总览：[`docs/框架总览.md`](docs/框架总览.md)
- 使用手册：[`docs/使用手册.html`](docs/使用手册.html)
- 框架介绍与代码讲解：[`docs/框架介绍与代码讲解.html`](docs/框架介绍与代码讲解.html)
- 新设备注册指南：[`docs/新设备注册指南.html`](docs/新设备注册指南.html)

## 核心结构

### 1. 设备模型

- `src/framework/include/fw/dev.h`：设备核心，保存“路径 → 设备”，提供
  `fw_dev_register/find/open/close/read/write/ioctl/isr`。
- `pin`：一个设备覆盖所有 GPIO，`offset = (port << 4) | pin`。
- `serial1..N`：每个启用的 UART 一个设备，名字由 Kconfig 决定。
- `spi1..N`：每个启用的 SPI 一个设备；同步 `fw_spi_transfer` 与
  `fw_spi_transfer_async/wait/abort` 共用同一套 ops，后三者由驱动用
  RT-Thread 信号量实现（类层保持 OS 中立）。
- `i2c1..3`：主模式阻塞事务；`off` 是 7 位从机地址，
  `mem_read/mem_write` 支持寄存器地址和 repeated start。
- `adc1`：单通道阻塞采样；`off` 是 ADC 通道，`fw_adc_read_mv` 负责 mV 换算。
- `pwm4`：TIM4 四路 PWM；频率属于定时器配置，`off` 是通道 0..3，
  占空比使用千分比，启停为全局操作。
- `lcd`：ST7735S，使用 `spiN` + `pin` 设备。
- 路径支持 `serial1` 和 `/dev/serial1`；当前是静态扁平注册表，不依赖 malloc。
- SPI DMA 打开后，长度 >= 16 的同步传输自动走 DMA；更短命令保持轮询。

### 2. 任务与健康度

- `src/framework/include/fw/task.h`：OS 中立的任务表、健康度、摘要/详情/控制 API。
- `src/framework/components/task/fw_task.c`：注册、心跳、栈高水位、CPU 窗口和告警判定。
- `src/framework/port/rtthread/rtthread_task.c`：静态 `rt_thread` 槽位、状态/栈快照、挂起/恢复/改优先级。
- `src/app/task/app_task.c`：main 作为监督者，adopt 自身，创建并启动 `rpc` 与可控 `worker` 任务。
- `main` 周期调用 `fw_task_refresh`；RPC 任务在每轮循环调用 `fw_task_heartbeat_self`。
- 健康指标：栈剩余、CPU 占用、线程状态、心跳 warn/lost；异常只标记并上报，不自动重启任务。
- 挂起是协作式的：control 只置 pending，目标任务在 `heartbeat_self` 中自行挂起；`main` 不可控。
- IWDG 可选：`BOARD_IWDG_ENABLE=y` 时 main 只在没有任务 LOST 时喂狗；
  任务失联或 main 卡死会触发复位，复位原因在启动横幅中打印。
- Fault 现场：HardFault 走 RT-Thread 异常钩子，MemManage/BusFault/UsageFault
  有强符号入口；输出栈帧、CFSR/HFSR/MMFAR/BFAR，并按 `BOARD_FAULT_RESET` 复位或停机。
- 日志：`fw_log` 提供 `[E]/[W]/[I]/[D]` 分级输出，`FW_LOG_LEVEL` 做编译期裁剪；
  RT-Thread 端口绑定到 `rt_hw_console_output`。
- 每个设备有累计 `error_count`，类层在驱动返回负值时递增，可用
  `fw_dev_error_count()` 读取。
- NVM/参数：板级尾部 256KB 分成两个 128KB 扇区，`fw_param` 使用 A/B 槽、
  CRC32 和递增序号；掉电写坏一个槽会回退到另一个槽。
- RPC：`0x05` 摘要、`0x06` 按索引详情、`0x07` 控制。

### 3. Platform / board

- `src/platform/stm32f407/`：BSP 驱动、映射、数据驱动时钟、HAL/NVIC/SysTick、console、panic。
- `drv_serial.c`/`drv_spi.c` 接收板级 `drv_*_hw_t` 描述符，不直接读 `BOARD_*` 引脚。
- 板级只有一个组合根 `board.c` 和一个接线事实源 `board_config.h`：
  - `board_config.h` 是纯 C：身份、内存/NVM、逻辑引脚、UART/SPI/I2C/ADC/PWM
    通道、DMA 路由、LCD 面板参数；
  - `board.c` 把 C 表按 Kconfig 启用项转换成 `drv_*_hw_t`，注册设备、绑定
    控制台、配置时钟、初始化 NVM 与 LCD，并在启动前校验接线；
  - `board.h` 只暴露应用需要的板级能力。
- `src/platform/board/stm32f407ve/Kconfig.board`：只保留时钟源、启用项、波特率、
  IRQ 优先级、SPI/I2C/ADC/PWM 参数、IWDG/fault 等构建策略，生成
  `board_policy.h`。
- `link.lds` 内联 Flash 代码区/NVM/RAM/CCM/最小栈，不再包含生成的内存脚本。
- Keil 工程从 `board_config.h` + `board_policy.h` 派生 Device/IROM/IRAM/Stack/CLOCK。
- 启动画面由 `src/app/boot/app_boot.c` 负责，board 不依赖头像资源。

### 4. 单一配置源

```text
src/platform/board/stm32f407ve/board_config.h   手写板级接线事实源
profiles/host/{defconfig,.config,generated/}
profiles/f407ve/{defconfig,.config,generated/}
profiles/f407ve-hse/{defconfig,.config,generated/}
profiles/f407ve-dma/{defconfig,.config,generated/}
```

- `board_config.h` 是引脚/AF/内存/LCD/NVM 的唯一来源；
- Kconfig 只保留启用项、容量、栈、ring、IRQ 优先级、时钟源、启动画面等构建选择；
- CMake 与 Keil 共同消费 `profiles/<profile>/generated/`（`board_policy.h` 等）；
- 没有 `profile.json`、`boardgen`、`boards/`、`chips/`；
  也没有 `build/<preset>/.config`、`keil/.config` / `keil/generated`。

```text
python tools/fwconfig.py sync --all
python tools/fwconfig.py menuconfig --profile f407ve
python tools/fwconfig.py check --all
python tools/fwconfig.py check-layout
```

Windows 下可直接双击 `tools\menuconfig.cmd`（默认 profile `f407ve`），或运行
`tools\menuconfig.cmd host` 切换 profile。

### 5. 单一源文件清单

`project/sources.json` 同时驱动 CMake 和 `tools/gen_keil.py`；Keil 工程由
source manifest + `keil/project_template.uvprojx` + `board_config.h` 生成。
新增 C 源文件改 `project/sources.json`；新增板级接线改 `board_config.h`。

## 快速开始

```text
cmake --preset host-debug
cmake --build --preset host-debug
ctest --preset host-debug --output-on-failure

cmake --preset f407-debug
cmake --build --preset f407-debug

cmake --preset f407-hse-debug
cmake --build --preset f407-hse-debug

# UART1 RX/TX DMA + SPI1 TX DMA + pin EXTI 验证配置（默认固件不启用）
cmake --preset f407-dma-debug
cmake --build --preset f407-dma-debug
```

产物：

```text
build/f407-debug/cmake/fw_v2.elf/.hex/.bin
```

Host CTest 当前 8/8：Host 单元测试（含板级 C 表结构）、profile 一致性、
布局检查、源文件/卫生检查、静态板级一致性单测、HSI/HSE Keil 工程生成检查。

## Keil MDK（AC5）

```text
keil/stm32f407ve.uvprojx      # profiles/f407ve (HSI 16 MHz)
keil/stm32f407ve-hse.uvprojx  # profiles/f407ve-hse (HSE 168 MHz)
```

Device/Vendor/Flash 算法/IRAM/IROM/Stack 来自 `board_config.h`，CLOCK 来自
时钟源选择，不再有 `board_keil.json`。工程仍需在 UV4 中实编译复验
（`gen-keil --check` 只证明文件同步）。

修改配置或源文件后：

```text
python tools/fwconfig.py sync --profile f407ve
python tools/fwconfig.py gen-keil --profile f407ve
```

打开工程，Target 选 `Debug`，F7 编译；产物在 `build/keil/`。下载器设置和
烧录步骤见 [`keil/KEIL.md`](keil/KEIL.md)。

## Kconfig 配置

板级身份/内存/引脚/AF/通道/LCD 面板参数/NVM 分区不在 Kconfig 中，
见 `src/platform/board/stm32f407ve/board_config.h`。

| 选项 | 默认 | 说明 |
|---|---|---|
| `BOARD_CLOCK_HSI_16` | y | HSI 16 MHz（无外部晶振） |
| `BOARD_CLOCK_HSE_8_168` | n | HSE 8 MHz → 168 MHz |
| `BOARD_LED_ENABLE` | y | 启用板载 LED |
| `BOARD_LCD_ENABLE` | y | ST7735S 显示设备 |
| `BOARD_UART_BAUD` | 115200 | 所有 UART 默认波特率 |
| `BOARD_UART_IRQ_PRIORITY` | 6 | UART 中断优先级 |
| `BOARD_DMA_IRQ_PRIORITY` | 7 | DMA stream 优先级 |
| `BOARD_EXTI_IRQ_PRIORITY` | 8 | EXTI 优先级 |
| `BOARD_FAULT_RESET` | n | Fault/panic 输出后是否复位 |
| `BOARD_IWDG_ENABLE` | n | 独立看门狗（`f407ve-dma` 打开） |
| `BOARD_IWDG_TIMEOUT_MS` | 5000 | IWDG 超时，需大于心跳 lost + 刷新周期 |
| `FW_LOG_LEVEL` | 3 | 日志编译期上限（0 关，4 debug） |
| `FW_USE_PARAM` | y | 参数存储组件（隐藏开关） |
| `FW_PARAM_DATA_MAX` | 1024 | 单份参数最大字节数 |
| `BOARD_SPI1..3_USE_MISO` | SPI1=n | 是否使用 MISO |
| `BOARD_SPI1..3_MAX_HZ` | 12 MHz | SPI 频率上限 |
| `FW_USING_UART1..3` | UART1=y | 逐实例启用 `serialN` |
| `FW_USING_UARTn_DMA_RX/TX` | n | UART RX/TX DMA（`f407ve-dma` 打开 UART1） |
| `FW_SERIAL_DMA_CHUNK` | 128 | UART DMA 半块/staging 大小 |
| `FW_USING_PIN_EXTI` | n | pin EXTI 驱动（`f407ve-dma` 打开） |
| `FW_PIN_IRQ_MAX` | 8 | pin 回调表容量 |
| `FW_USING_SPI1..3` | SPI1=y | 逐实例启用 `spiN` |
| `FW_USING_SPI1..3_DMA` | n | SPI DMA（`f407ve-dma` 打开 SPI1 TX） |
| `FW_USING_I2C1..3` | n（目标 profile 打开 I2C1） | 逐实例启用阻塞主模式 `i2cN` |
| `BOARD_I2C1..3_SPEED_HZ` | 100 kHz | I2C 总线频率（最大 400 kHz） |
| `FW_USING_ADC1` | n（目标 profile 打开） | 启用 `adc1` 单通道轮询采样 |
| `BOARD_ADC1_VREF_MV` | 3300 | ADC 参考电压 |
| `BOARD_ADC1_RESOLUTION_BITS` | 12 | ADC 分辨率（8/10/12） |
| `BOARD_ADC1_SAMPLE_CYCLES` | 84 | ADC 采样周期 |
| `FW_USING_PWM4` | n（目标 profile 打开） | 启用 TIM4 四路 PWM `pwm4` |
| `BOARD_PWM4_PERIOD_HZ` | 1000 | PWM 频率 |
| `FW_SERIAL_RX_BUFSZ` | 512 | 每实例 RX ring |
| `FW_SERIAL_TX_BUFSZ` | 512 | 每实例 TX ring |
| `APP_BOOT_DISPLAY` | y | 是否绘制启动画面 |
| `APP_BOOT_BARS/IMAGE/WHITE` | IMAGE | 色条 / 头像 / 纯白 |
| `FW_TASK_MAX` | 8 | 任务表容量 |
| `APP_RPC_TASK_STACK_SIZE` | 2048 | RPC 任务静态栈 |
| `APP_RPC_TASK_PRIORITY` | 9 | RPC 任务优先级（main=10） |
| `APP_HEALTH_REFRESH_MS` | 1000 | 健康刷新周期 |
| `APP_TASK_HEARTBEAT_WARN/LOST_MS` | 1500/3000 | 心跳 warn/lost 阈值 |
| `APP_TASK_STACK_WARN_BYTES` | 256 | 栈剩余告警阈值 |
| `RT_USING_HEAP` | n | 默认全静态分配 |
| `RT_USING_CONSOLE` | y | RT-Thread banner 与 `rt_kprintf` |

### 增加外设实例或改接线

- 已有实例：只改 Kconfig 的 `FW_USING_UARTn`/`FW_USING_SPIn`/
  `FW_USING_I2Cn`/`FW_USING_ADC1`/`FW_USING_PWM4`；
- 新增实例或改引脚/AF/通道：编辑 `board_config.h` 的 C 表；
- I2C 固定为开漏 + 内部上拉；400 kHz 需要板级外部上拉；
- ADC 通道条目只有 channel/port/pin，引脚必须匹配 `ADC1_INn`；
- 控制台/LED/LCD 绑定分别是 `BOARD_CONSOLE_UART`、`BOARD_PIN_LED`、
  `BOARD_LCD_INIT`。

```text
python tools/fwconfig.py sync --all
python tools/fwconfig.py gen-keil --profile f407ve
python tools/fwconfig.py gen-keil --profile f407ve-hse
cmake --build --preset f407-debug
```

## 目录

| 目录 | 职责 |
|---|---|
| `src/app/` | 唯一业务层：main、rpc、boot、task、assets |
| `src/framework/include/fw/` | dev/pin/serial/spi/i2c/adc/pwm/display/protocol/st7735s/task/clock/ring/status |
| `src/framework/core/` | `fw_*.c` 设备核心与类实现 |
| `src/framework/components/protocol/` | A5 5A 帧编解码与 CRC |
| `src/framework/components/display/` | ST7735S 面板驱动 |
| `src/framework/components/task/` | 任务表、健康度、CPU 窗口 |
| `src/framework/port/rtthread/` | RT-Thread tick 与静态线程适配 |
| `src/platform/stm32f407/` | BSP 驱动、映射、时钟、boot/console/IRQ/panic、HAL 配置 |
| `src/platform/board/stm32f407ve/` | board.c 组合根、board_config.h 接线表、link.lds、BOARD.md |
| `src/tests/host/` | Host 假设备/假任务、framework/task/app_rpc/板级表测试 |
| `src/tests/hil/` | 串口协议 HIL |
| `profiles/` | defconfig/.config + generated 策略头 |
| `project/sources.json` | CMake 与 Keil 共用源文件清单 |
| `keil/` | AC5 模板、生成工程、启动文件、scatter |
| `docs/` | 架构说明与 HTML 使用手册 |
| `src/vendor/rt-thread-nano/` | 官方 Nano 3.1.5 |
| `src/third_party/stm32f4/` | HAL/CMSIS/device 子集 |

## 线协议

```text
A5 5A LEN SEQ CMD PAYLOAD CRC_LO CRC_HI
0x01 PING, 0x02 LED_SET, 0x03 STATUS, 0x04 STATS
0x05 TASK_SUMMARY, 0x06 TASK_DETAIL, 0x07 TASK_CONTROL
0x90 event
```

- `0x05`：13 字节摘要（total/ok/warn/lost、worst/current、busy/idle/other）。
- `0x06`：请求 1 字节索引；回复 31 字节详情（状态、健康、栈、CPU、心跳）。
- `0x07`：请求 `index/action/value`；suspend 为协作式 pending。

HIL：

```text
python src/tests/hil/test_protocol.py --port COMx --stats --tasks
```

## 内核来源

`src/vendor/rt-thread-nano/` 来自官方 `RealThread.RT-Thread.3.1.5.pack`，包含
GCC 14 的 4 处 IT-block 补丁；详见
[`src/vendor/rt-thread-nano/UPSTREAM.md`](src/vendor/rt-thread-nano/UPSTREAM.md)。

## 边界

- 内核为 RT-Thread Nano 3.1.5；无 FinSH/msh、ulog、RT-Thread device framework。
- 默认 UART IT；`f407ve-dma` 可打开 UART1 RX/TX DMA、SPI1 TX DMA 与 pin EXTI。
  DMA/EXTI 的电气与时序行为仍需上板 HIL。
- SPI DMA 支持同步与 async/wait/abort；SPI1 的 MISO（PA6）与 LCD DC 共用，
  因此默认 profile 只能 TX-only。全双工需要 SPI2/SPI3 或修改接线。
- I2C/ADC/PWM 首版全部阻塞/轮询：I2C 主模式事务、ADC 单通道单次转换、
  TIM4 PWM 四路输出；不使用中断/DMA。I2C 400 kHz 与 ADC 精度需按硬件复验。
- ADC 未调用校准 API（当前 HAL/CMSIS 子集不提供）；PWM 在 ARR=65535 时
  100% 占空比会钳位到约 99.998%。
- IWDG 使用 LSI 32kHz 估算，实际超时随芯片偏差浮动；启动后不可停止，
  长时间断点调试时应关闭 `BOARD_IWDG_ENABLE`。
- Fault dump 在控制台已绑定时可用；故障发生在串口初始化之前只能停机，
  无法输出文本。日志缓冲不可重入，只在任务上下文使用。
- NVM 使用两个 128KB 扇区做 A/B 槽；每次 save 擦除整个非活动槽，
  适合低频参数，不适合高频写入。没有更复杂的磨损均衡。
- 构建链：CMake + ARM GCC；Keil MDK + ARMCC 5。AC6 未适配。
- 真实硬件 HIL 需要板卡；未上板前不声称硬件验证完成。
