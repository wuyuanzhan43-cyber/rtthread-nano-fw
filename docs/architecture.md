# 架构说明（整改后）

## 1. 分层

```text
src/app/main.c
  ├─ src/board/stm32f407ve（时钟、引脚编号、注册）
  │    ├─ src/bsp/st/driver（drv_pin / drv_serial / drv_spi）
  │    └─ src/bsp/st/stm32f407/driver/mr_board.h（端口/引脚/IRQ 表）
  ├─ src/framework/components（protocol / display）
  └─ src/framework/core/dev（设备注册表 + class）

src/framework/core/dev.c → fw/dev.h（不认识 pin/serial/spi/display）
src/vendor/rt-thread-nano → 生成的 rtconfig.h
```

依赖红线：

1. `src/framework/core` 与 `components` 不包含 HAL / RT-Thread 头文件。
2. `core/dev.c` 只保存路径、类型令牌、ops、drv；不认识具体设备类。
3. `bsp` 只实现 class ops；`board` 只做时钟、引脚编号和注册。
4. ISR 只碰 ring/事件；不在 ISR 调用阻塞 API。
5. 默认静态分配，`RT_USING_HEAP=n`。

## 2. mr 风格设备核心

```c
struct fw_dev_ops {
    open, close, read, write, ioctl, isr
};
struct fw_drv { int type; const void *ops; void *data; };
struct fw_dev { char name[]; int type; size_t ref_count;
                const fw_dev_ops *ops; const fw_drv *drv; };
```

- 注册：`fw_dev_register(dev, "serial1", type, drv)`。
- 查找：`fw_dev_find("serial1")` 或 `fw_dev_find("/dev/serial1")`。
- 打开/关闭维护 `ref_count`；read/write 使用 `offset` 语义。
- 当前是静态扁平注册表；没有 mr-library 的动态设备树、descriptor、读写锁。

### 与 mr-library 的对应

| mr-library | 本项目 | 说明 |
|---|---|---|
| `mr_dev_*` | `fw_dev_*` | 保留 open/close/read/write/ioctl/isr 语义 |
| `mr_pin` + 引脚编号 | `pin` + `(port<<4)|pin` | 一个设备覆盖所有 GPIO |
| `mr_serial` + serialN | `serialN` | 每实例一个设备，ring 静态 |
| `drv_pin.c` + `mr_board.h` | 同名/同结构 | 端口/引脚/IRQ 表 |
| `MR_INIT_*_EXPORT` | 显式 board 注册 | 避免链接段差异，启动顺序更直观 |
| `mr_malloc` ring | 静态 ring | 无堆、RAM 可预测 |

不直接 vendor mr-library 的原因：它的 ring/pin IRQ 依赖 malloc，`mr_config.h`
会成为第二配置源，且缺少本项目需要的 wait_rx 语义。

## 3. 设备类

| 类 | 设备名 | 关键语义 |
|---|---|---|
| pin | `pin` | offset 是引脚号；`FW_CTL_PIN_SET_MODE` 配置模式 |
| serial | `serial1..N` | RX/TX ring；`GET/SET_CONFIG`、`CLR_*`、`GET_*DATASZ` |
| spi | `spi1..N` | `SET/GET_CONFIG`，transfer 由 class helper 调用 |
| display | `lcd` | 窗口/像素/背光；通用裁剪由 core 完成 |

serial 的 ring 由 driver/board 提供静态存储；driver 通过 `ops->lock/unlock`
注入 RT-Thread 临界区，core 不直接依赖 RTOS。

## 4. BSP / board

```text
src/bsp/st/stm32f407/driver/
  mr_board.h: DRV_PIN_PORT_CONFIG / DRV_PIN_CONFIG / DRV_PIN_IRQ_MAP_CONFIG
  Kconfig:    FW_USING_UARTn / FW_USING_SPIn
```

- `drv_pin.c`：一个 `pin` 设备；使用 `mr_board.h` 查端口和引脚。
- `drv_serial.c`：`#if FW_USING_UARTn` 生成 `serialN`；USART IRQ 在驱动内；
  RX/TX ring + `rt_event`；console 实例由 `BOARD_CONSOLE_UART_NUM` 选择。
- `drv_spi.c`：`#if FW_USING_SPIn` 生成 `spiN`；引脚来自 board_config。
- `board_devices.c`：`drv_pin_register`、`drv_serial_register_all`、
  `drv_spi_register_all`、LCD 注册；不持有每个外设对象。
- `board_irq.c`：只保留 SysTick。

## 5. 配置

```text
profiles/<profile>/defconfig
profiles/<profile>/.config
profiles/<profile>/generated/
```

- `fwconfig.py sync`：从 defconfig 初始化 `.config`，再生成头文件。
- CMake 的 `FW_PROFILE` 和 Keil 都使用同一个 profile；没有 per-build/Keil 配置。
- `project/sources.json` 是 CMake 与 Keil 的唯一源文件清单。
- `tools/gen_keil.py` 从 manifest + `keil/project_template.uvprojx` 生成工程。
- `fwconfig.py check --all`、`check-layout`、`check-manifest`、`gen-keil --check`
  由 Host CTest 执行。

## 6. 启动与并发

```text
rt_hw_board_init
  HAL_Init → clock → SysTick → board_devices_init
  pin/serial/spi 注册；LCD 注册
main
  fw_dev_open(console/pin) → board_console_start
  LCD setup → demo init → poll + fw_serial_wait_rx
```

- SysTick：`rt_interrupt_enter → HAL_IncTick → rt_tick_increase → rt_interrupt_leave`。
- serial RX：RXNE ISR → class ring → `rt_event_send`；任务 wait/poll。
- serial TX：任务写 ring → 驱动 start_tx → TXE ISR 搬运。
- `fw_serial` 的 ring 访问通过 driver lock/unlock（RT-Thread PRIMASK）保护。
- LCD/SPI 只在 main 线程使用；首版不做多任务共享锁。

## 7. 验证现状

- Host CTest 5/5。
- F407 GCC：默认配置构建通过（约 82 KB Flash / 10.9 KB RAM）。
- Keil AC5：0 error / 0 warning（Code 27,844 / RO 42,076 / RW 164 / ZI 11,276）。
- Kconfig-only：启用 UART2/SPI2、LED 改 PE3，两种工具链均构建通过，产物含
  `serial2`、`spi2`、`USART2_IRQHandler`。
- 真实硬件 HIL 仍需上板执行。
