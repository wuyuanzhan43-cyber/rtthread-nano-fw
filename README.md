# Embedded Framework v2 · RT-Thread Nano 3.1.5

独立工程，不依赖旧版目录或外部 checkout。

```text
src/app（main 组合根 + rpc 业务 + boot 启动画面 + assets）
        │
src/platform/board/stm32f407ve（时钟/接线描述、验证、实例装配、内存布局）
        │
src/platform/stm32f407（pin/serial/spi 驱动、映射、HAL 配置）
        │
src/framework（mr 风格设备核心 + 设备类 + 协议 + 面板驱动 + 任务管理 + RT-Thread 端口）
        │
RT-Thread Nano（调度/IPC/时间基）
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

### 2. 任务与健康度

- `src/framework/include/fw/task.h`：OS 中立的任务表、健康度、摘要/详情/控制 API。
- `src/framework/components/task/fw_task.c`：注册、心跳、栈高水位、CPU 窗口和告警判定。
- `src/framework/port/rtthread/rtthread_task.c`：静态 `rt_thread` 槽位、状态/栈快照、挂起/恢复/改优先级。
- `src/app/task/app_task.c`：main 作为监督者，adopt 自身，创建并启动 `rpc` 与可控 `worker` 任务。
- `main` 周期调用 `fw_task_refresh`；RPC 任务在每轮循环调用 `fw_task_heartbeat_self`。
- 健康指标：栈剩余、CPU 占用、线程状态、心跳 warn/lost；异常只标记并上报，不自动重启。
- 挂起是协作式的：control 只置 pending，目标任务在 `heartbeat_self` 中自行挂起；`main` 不可控。
- RPC：`0x05` 摘要、`0x06` 按索引详情、`0x07` 控制。

### 3. Platform / board

- `src/platform/stm32f407/`：BSP 驱动、映射、数据驱动时钟、HAL/NVIC/SysTick、console、panic。
- `drv_serial.c`/`drv_spi.c` 接收板级 `drv_*_hw_t` 描述符，不直接读 `BOARD_*` 引脚。
- `chips/stm32f407.json`：芯片/封装、内存、AF 矩阵、DMA 请求、EXTI 分组、Keil 器件信息。
- `boards/stm32f407ve/board.json`：板卡身份、时钟 variant、命名引脚、外设接线、设备绑定。
- `profiles/<profile>/profile.json`：只选择 board/chip/clock variant，不复制事实。
- `tools/boardgen.py`：manifest 校验、兼容 `BOARD_*` 宏、`board_desc.h`/`board_memory.ld`/`board_summary.txt`/`board_keil.json` 生成。
- `src/platform/board/stm32f407ve/Kconfig.board`：只保留启用项、波特率、IRQ 优先级、SPI 频率、最小栈等构建策略。
- `board_pins.c/h`：串口/SPI 描述符与启动前校验；AF/引脚/DMA 路由来自 manifest 生成的 `BOARD_*` 宏。
- `board_clock.c/h`：HSI/HSE 的 `stm32f407_clock_cfg_t`。
- `board_devices.c`：pin/LED/serial/SPI/LCD 注册、控制台绑定；不持有 pin/serial/spi 实例，但持有 LCD 对象。
- `board.c` 只做 platform boot → 设备注册 → 组件初始化；SysTick/console/panic 不在 board 目录。
- 内存区域由 manifest 生成 `board_memory.ld`；GCC/Keil scatter/Keil 器件字段共用 `board_keil.json`。
- 启动画面由 `src/app/boot/app_boot.c` 负责，board 不依赖头像资源。

### 4. 单一配置源

```text
chips/stm32f407.json
boards/stm32f407ve/board.json
profiles/host/{profile.json,defconfig,.config,generated/}
profiles/f407ve/{profile.json,defconfig,.config,generated/}
profiles/f407ve-hse/{profile.json,defconfig,.config,generated/}
```

- manifest 是引脚/AF/时钟/内存/设备绑定的唯一来源；
- Kconfig 只保留启用项、容量、栈、ring、IRQ 优先级、启动画面等构建选择；
- CMake 与 Keil 共同消费 `profiles/<profile>/generated/`；
- 没有 `build/<preset>/.config`，也没有 `keil/.config` / `keil/generated`。

```text
python tools/fwconfig.py sync --all
python tools/fwconfig.py menuconfig --profile f407ve
python tools/fwconfig.py check --all
python tools/fwconfig.py check-boards
```

Windows 下可直接双击 `tools\menuconfig.cmd`（默认 profile `f407ve`），或运行
`tools\menuconfig.cmd host` 切换 profile。

### 5. 单一源文件清单

`project/sources.json` 同时驱动 CMake 和 `tools/gen_keil.py`；Keil 工程由
source manifest + `keil/project_template.uvprojx` + `board_keil.json` 生成。
新增 C 源文件改 `project/sources.json`；新增板级接线改 board/chip manifest。

## 快速开始

```text
cmake --preset host-debug
cmake --build --preset host-debug
ctest --preset host-debug --output-on-failure

cmake --preset f407-debug
cmake --build --preset f407-debug

cmake --preset f407-hse-debug
cmake --build --preset f407-hse-debug

# UART1 DMA + pin EXTI 验证配置（默认固件不启用）
cmake --preset f407-dma-debug
cmake --build --preset f407-dma-debug
```

产物：

```text
build/f407-debug/cmake/fw_v2.elf/.hex/.bin
```

Host 测试当前 9/9：Host 单元测试、profile 一致性、布局检查、源文件/layout/卫生检查、
board manifest 校验、生成器单测、HSI/HSE Keil 工程生成检查。

## Keil MDK（AC5）

```text
keil/stm32f407ve.uvprojx      # profiles/f407ve (HSI 16 MHz)
keil/stm32f407ve-hse.uvprojx  # profiles/f407ve-hse (HSE 168 MHz)
```

Device/Vendor/Cpu/Flash 算法/IRAM/IROM/CLOCK 现在来自 `board_keil.json`，
不再手写。工程仍需在 UV4 中实编译复验（`gen-keil --check` 只证明文件同步）。

修改配置或源文件后：

```text
python tools/fwconfig.py sync --profile f407ve
python tools/fwconfig.py gen-keil --profile f407ve
```

打开工程，Target 选 `Debug`，F7 编译；产物在 `build/keil/`。下载器设置和
烧录步骤见 [`keil/KEIL.md`](keil/KEIL.md)。

## Kconfig 配置

板级身份/时钟/内存/引脚/AF/LCD 面板参数不在 Kconfig 中，见 board/chip manifest。

| 选项 | 默认 | 说明 |
|---|---|---|
| `BOARD_MIN_STACK_SIZE` | 4096 | 主栈/中断栈预留 |
| `BOARD_LED_ENABLE` | y | 启用板载 LED |
| `BOARD_LCD_ENABLE` | y | ST7735S 显示设备 |
| `BOARD_UART_BAUD` | 115200 | 所有 UART 默认波特率 |
| `BOARD_UART_IRQ_PRIORITY` | 6 | UART 中断优先级 |
| `BOARD_DMA_IRQ_PRIORITY` | 7 | DMA stream 优先级 |
| `BOARD_EXTI_IRQ_PRIORITY` | 8 | EXTI 优先级 |
| `BOARD_SPI1..3_USE_MISO` | SPI1=n | 是否使用 MISO |
| `BOARD_SPI1..3_MAX_HZ` | 12 MHz | SPI 频率上限 |
| `FW_USING_UART1..3` | UART1=y | 逐实例启用 `serialN` |
| `FW_USING_UARTn_DMA_RX/TX` | n | UART RX/TX DMA（`f407ve-dma` 打开 UART1） |
| `FW_SERIAL_DMA_CHUNK` | 128 | UART DMA 半块/staging 大小 |
| `FW_USING_PIN_EXTI` | n | pin EXTI 驱动（`f407ve-dma` 打开） |
| `FW_PIN_IRQ_MAX` | 8 | pin 回调表容量 |
| `FW_USING_SPI1..3` | SPI1=y | 逐实例启用 `spiN` |
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

### 增加 UART/SPI 实例或改接线

- 已有实例：只改 Kconfig 的 `FW_USING_UARTn`/`FW_USING_SPIn`；
- 新增实例或改引脚/AF：编辑 `boards/stm32f407ve/board.json` 与可选
  `chips/stm32f407.json` 的 pinmux/DMA 表；
- 控制台/LED/LCD 绑定同样在 board manifest 的 `devices`/`named_pins` 中。

```text
python tools/fwconfig.py sync --all
python tools/fwconfig.py check-boards
python tools/fwconfig.py gen-keil --profile f407ve
python tools/fwconfig.py gen-keil --profile f407ve-hse
cmake --build --preset f407-debug
```

## 目录

| 目录 | 职责 |
|---|---|
| `src/app/` | 唯一业务层：main、rpc、boot、task、assets |
| `src/framework/include/fw/` | dev/pin/serial/spi/display/protocol/st7735s/task/clock/ring/status |
| `src/framework/core/` | `fw_*.c` 设备核心与类实现 |
| `src/framework/components/protocol/` | A5 5A 帧编解码与 CRC |
| `src/framework/components/display/` | ST7735S 面板驱动 |
| `src/framework/components/task/` | 任务表、健康度、CPU 窗口 |
| `src/framework/port/rtthread/` | RT-Thread tick 与静态线程适配 |
| `src/platform/stm32f407/` | BSP 驱动、映射、时钟、boot/console/IRQ/panic、HAL 配置 |
| `src/platform/board/stm32f407ve/` | 板级装配/校验、link.lds、BOARD.md |
| `chips/` | 芯片/封装 manifest（内存、AF、DMA、EXTI、Keil 器件信息） |
| `boards/` | 板卡 manifest（身份、时钟 variant、接线、设备绑定） |
| `src/tests/host/` | Host 假设备/假任务、framework 测试、task 测试、app_rpc 测试 |
| `src/tests/hil/` | 串口协议 HIL |
| `profiles/` | profile.json 选择器 + defconfig/.config + generated |
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
- 默认 UART IT；`f407ve-dma` 可打开 UART1 RX/TX DMA 与 pin EXTI。
  DMA/EXTI 的电气与时序行为仍需上板 HIL；SPI DMA 尚未实现。
- 构建链：CMake + ARM GCC；Keil MDK + ARMCC 5。AC6 未适配。
- 真实硬件 HIL 需要板卡；未上板前不声称硬件验证完成。
