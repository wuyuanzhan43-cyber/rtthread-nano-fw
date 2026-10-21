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
- `src/app/task/app_task.c`：main 作为监督者，adopt 自身，创建并启动独立的 `rpc` 任务。
- `main` 周期调用 `fw_task_refresh`；RPC 任务在每轮循环调用 `fw_task_heartbeat_self`。
- 健康指标：栈剩余、CPU 占用、线程状态、心跳 warn/lost；异常只标记并上报，不自动重启。
- 挂起是协作式的：control 只置 pending，目标任务在 `heartbeat_self` 中自行挂起；`main` 不可控。
- RPC：`0x05` 摘要、`0x06` 按索引详情、`0x07` 控制。

### 3. Platform / board

- `src/platform/stm32f407/`：BSP 驱动、映射、数据驱动时钟、HAL/NVIC/SysTick、console、panic。
- `drv_serial.c`/`drv_spi.c` 接收板级 `drv_*_hw_t` 描述符，不再直接读 `BOARD_*` 引脚。
- `src/platform/board/stm32f407ve/Kconfig.board`：时钟、板卡信息、内存、LED/控制台/UART/SPI/LCD 接线。
- `board_pins.c/h`：串口/SPI 描述符 + 端口/AF/SWD/重复引脚校验。
- `board_clock.c/h`：HSI/HSE 的 `stm32f407_clock_cfg_t`。
- `board_devices.c`：pin/LED/serial/SPI/LCD 注册、控制台绑定；不持有 pin/serial/spi 实例，但持有 LCD 对象。
- `board.c` 只做 platform boot → 设备注册 → 组件初始化；SysTick/console/panic 不再在 board 目录。
- 内存区域由 `Kconfig.board` 生成 `board_memory.ld`，GCC 与 Keil scatter 共用同一来源。
- 启动画面由 `src/app/boot/app_boot.c` 负责，board 不再依赖头像资源。

### 4. 单一配置源

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

### 5. 单一源文件清单

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

Host 测试当前 6/6：Host 单元测试、profile 一致性、布局检查、manifest 检查、
Keil 工程生成检查、命名/旧路径卫生检查。

## Keil MDK（AC5）

```text
keil/stm32f407ve.uvprojx
```

重构前基线已用 UV4 + ARMCC 5.06 update 5 验证；本次重构后工程已由 manifest 重新生成，需在 UV4 中复验。

修改配置或源文件后：

```text
python tools/fwconfig.py sync --profile f407ve
python tools/fwconfig.py gen-keil --profile f407ve
```

打开工程，Target 选 `Debug`，F7 编译；产物在 `build/keil/`。下载器设置和
烧录步骤见 [`keil/KEIL.md`](keil/KEIL.md)。

## Kconfig 配置

| 选项 | 默认 | 说明 |
|---|---|---|
| `BOARD_NAME` / `BOARD_VERSION` | STM32F407VE / 0.2.0 | 板卡身份 |
| `BOARD_CLOCK_HSI_16` | y | HSI 16 MHz，无需外部晶振 |
| `BOARD_CLOCK_HSE_8_168` | n | HSE → PLL → 168 MHz |
| `BOARD_HSE_VALUE` | 8 MHz | HSE 频率（选择 HSE 时） |
| `BOARD_FLASH/RAM/CCM_SIZE_KB` | 512/128/64 | 链接区域大小 |
| `BOARD_MIN_STACK_SIZE` | 4096 | 主栈/中断栈预留 |
| `BOARD_CONSOLE_UART1/2/3` | UART1 | 控制台串口选择 |
| `FW_USING_UART1..3` | UART1=y | 逐实例启用 `serialN` |
| `FW_USING_SPI1..3` | SPI1=y | 逐实例启用 `spiN` |
| `FW_SERIAL_RX_BUFSZ` | 512 | 每实例 RX ring |
| `FW_SERIAL_TX_BUFSZ` | 512 | 每实例 TX ring |
| `BOARD_UART_DMA` | n | 预留；当前必须为 n |
| `BOARD_LCD_ENABLE` | y | ST7735S 显示设备 |
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

### 只改 Kconfig 增加实例

重构前已验证场景：启用 UART2 + SPI2、UART2 映射 PD5/PD6、LED 改 PE3，不修改任何
C/H 即可构建。本次重构保留同一 Kconfig 机制，CMake 侧已由 Host/固件构建覆盖；
Keil 侧需在 UV4 中复验。

```text
python tools/fwconfig.py sync --profile f407ve
python tools/fwconfig.py gen-keil --profile f407ve
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
| `src/platform/board/stm32f407ve/` | 板级描述/校验、设备装配、link.lds、BOARD.md |
| `src/tests/host/` | Host 假设备/假任务、framework 测试、task 测试、app_rpc 测试 |
| `src/tests/hil/` | 串口协议 HIL |
| `profiles/` | 唯一配置源（host / f407ve / f407ve-hse） |
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
- 默认 UART IT；DMA 不在本轮范围，`BOARD_UART_DMA` 必须为 n。
- 构建链：CMake + ARM GCC；Keil MDK + ARMCC 5。AC6 未适配。
- 真实硬件 HIL 需要板卡；未上板前不声称硬件验证完成。
