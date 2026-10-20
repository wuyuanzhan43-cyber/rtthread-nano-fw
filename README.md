# Embedded Framework v2 · RT-Thread Nano 3.1.5

独立工程，不依赖旧版目录或外部 checkout。

```text
RT-Thread Nano（调度/IPC/时间基）
        │
src/framework/dev（mr 风格 open/close/read/write/ioctl/isr + pin/serial/spi/display）
        │
src/bsp/st/driver（按 Kconfig 注册 serialN/spiN，pin 单设备 + mr_board 引脚表）
        │
src/board/stm32f407ve（时钟、引脚编号、实例装配）
```

离线文档：

- 使用手册：[`docs/使用手册.html`](docs/使用手册.html)
- 框架介绍与代码讲解：[`docs/框架介绍与代码讲解.html`](docs/框架介绍与代码讲解.html)

## 核心结构

### 1. 设备模型

- `src/framework/include/fw/dev.h`：设备核心，保存“路径 → 设备”，提供
  `fw_dev_register/find/open/close/read/write/ioctl/isr`。
- `pin`：一个设备覆盖所有 GPIO，`offset = (port << 4) | pin`。
- `serial1..N`：每个启用的 UART 一个设备，名字由 Kconfig 决定。
- `spi1..N`：每个启用的 SPI 一个设备。
- `lcd`：ST7735S，使用 `spiN` + `pin` 设备。
- 路径支持 `serial1` 和 `/dev/serial1`；当前是静态扁平注册表，不依赖 malloc。

### 2. BSP / board

- `src/bsp/st/stm32f407/driver/mr_board.h`：端口、引脚、IRQ、实例映射。
- `src/bsp/st/driver/drv_pin.c`：一个 `pin` 设备，不再为每个引脚创建对象。
- `src/bsp/st/driver/drv_serial.c`：按 `FW_USING_UARTn` 注册 `serialN`，USART IRQ 在驱动内。
- `src/bsp/st/driver/drv_spi.c`：按 `FW_USING_SPIn` 注册 `spiN`。
- `src/board/stm32f407ve/board_devices.c`：只调用注册函数，不再手工持有每个外设对象。
- `board_irq.c`：只保留 SysTick；HardFault/PendSV 由 libcpu 提供。

### 3. 单一配置源

```text
profiles/host/{defconfig,.config,generated/}
profiles/f407ve/{defconfig,.config,generated/}
profiles/f407ve-hse/{defconfig,.config,generated/}
```

CMake 目标构建和 Keil AC5 共同消费 `profiles/f407ve/generated/`。没有
`build/<preset>/.config`，也没有 `keil/.config` / `keil/generated`。

```text
python tools/fwconfig.py sync --profile f407ve
python tools/fwconfig.py menuconfig --profile f407ve
python tools/fwconfig.py check --all
```

Windows 下可直接双击 `tools\menuconfig.cmd`（默认 profile `f407ve`），或运行
`tools\menuconfig.cmd host` 切换 profile。

### 4. 单一源文件清单

`project/sources.json` 同时驱动 CMake 和 `tools/gen_keil.py`；Keil 工程由
manifest + `keil/project_template.uvprojx` 生成。新增源文件只改 manifest。

## 快速开始

```text
cmake --preset host-debug
cmake --build --preset host-debug
ctest --preset host-debug --output-on-failure

cmake --preset f407-debug
cmake --build --preset f407-debug

cmake --preset f407-hse-debug
cmake --build --preset f407-hse-debug
```

产物：

```text
build/f407-debug/cmake/fw_v2.elf/.hex/.bin
```

Host 测试当前 5/5：Host 单元测试、profile 一致性、布局检查、manifest 检查、
Keil 工程生成检查。

## Keil MDK（AC5）

```text
keil/stm32f407ve.uvprojx
```

已用 UV4 + ARMCC 5.06 update 5 验证：0 Error(s), 0 Warning(s)。

修改配置或源文件后：

```text
python tools/fwconfig.py sync --profile f407ve
python tools/fwconfig.py gen-keil --profile f407ve
```

打开工程，Target 选 `Debug`，F7 编译；产物在 `build/keil/`。下载器设置和
烧录步骤见 [`keil/README.md`](keil/README.md)。

## Kconfig 配置

| 选项 | 默认 | 说明 |
|---|---|---|
| `BOARD_CLOCK_HSI_16` | y | HSI 16 MHz，无需外部晶振 |
| `BOARD_CLOCK_HSE_8_168` | n | HSE 8 MHz → 168 MHz |
| `BOARD_CONSOLE_UART1/2/3` | UART1 | 控制台串口选择 |
| `FW_USING_UART1..3` | UART1=y | 逐实例启用 `serialN` |
| `FW_USING_SPI1..3` | SPI1=y | 逐实例启用 `spiN` |
| `FW_SERIAL_RX_BUFSZ` | 512 | 每实例 RX ring |
| `FW_SERIAL_TX_BUFSZ` | 512 | 每实例 TX ring |
| `BOARD_UART_DMA` | n | 预留；当前必须为 n |
| `BOARD_LCD_ENABLE` | y | ST7735S 与启动画面 |
| `RT_USING_HEAP` | n | 默认全静态分配 |
| `RT_USING_CONSOLE` | y | RT-Thread banner 与 `rt_kprintf` |

### 只改 Kconfig 增加实例

已验证场景：启用 UART2 + SPI2、UART2 映射 PD5/PD6、LED 改 PE3，不修改任何
C/H，CMake 与 Keil 都能构建；产物中可见 `serial2`、`spi2`、`USART2_IRQHandler`。

```text
python tools/fwconfig.py sync --profile f407ve
python tools/fwconfig.py gen-keil --profile f407ve
cmake --build --preset f407-debug
```

## 目录

| 目录 | 职责 |
|---|---|
| `src/` | 唯一代码大目录：app/framework/bsp/board/tests/third_party/vendor |
| `profiles/` | 唯一配置源（host / f407ve / f407ve-hse） |
| `project/sources.json` | CMake 与 Keil 共用源文件清单 |
| `src/framework/include/fw/` | dev/pin/serial/spi/display/status/ring/clock |
| `src/framework/core/` | device core、class 包装、ring/status |
| `src/framework/components/protocol/` | A5 5A 协议 |
| `src/framework/components/display/` | ST7735S |
| `src/framework/app/` | demo 与头像 |
| `src/bsp/st/driver/` | drv_pin/drv_serial/drv_spi/drv_common |
| `src/bsp/st/stm32f407/driver/` | mr_board.h 引脚表 + 逐实例 Kconfig |
| `src/board/stm32f407ve/` | 时钟、注册、console、IRQ、链接脚本 |
| `keil/` | AC5 模板、生成工程、启动文件、scatter |
| `src/tests/host/` | Host 假设备与单测 |
| `src/tests/hil/` | 串口协议 HIL |
| `docs/` | 架构说明与 HTML 使用手册 |
| `src/vendor/rt-thread-nano/` | 官方 Nano 3.1.5 |
| `src/third_party/stm32f4/` | HAL/CMSIS/device 子集 |

## 线协议

```text
A5 5A LEN SEQ CMD PAYLOAD CRC_LO CRC_HI
0x01 PING, 0x02 LED_SET, 0x03 STATUS, 0x04 STATS, 0x90 event
```

HIL：

```text
python src/tests/hil/test_protocol.py --port COMx --stats
```

## 内核来源

`src/vendor/rt-thread-nano/` 来自官方 `RealThread.RT-Thread.3.1.5.pack`，包含
GCC 14 的 4 处 IT-block 补丁；详见
[`src/vendor/rt-thread-nano/UPSTREAM.md`](src/vendor/rt-thread-nano/UPSTREAM.md)。

## 边界

- 内核为 RT-Thread Nano 3.1.5；无 FinSH/msh、ulog、RT-Thread device framework。
- 默认 UART IT；DMA 不在本轮范围，`BOARD_UART_DMA` 必须为 n。
- 构建链：CMake + ARM GCC；Keil MDK + ARMCC 5。AC6 未适配。
- 真实硬件 HIL 需要板卡；未上板前不声称硬件验证完成。
