# 架构说明（整改后）

## 1. 分层

```text
src/app/main.c（监督者）
  ├─ src/app/task（adopt main、创建 rpc 任务）
  ├─ src/app/rpc（命令分发、上报）
  ├─ src/app/boot（启动画面）+ src/app/assets（头像）
  ├─ src/platform/board/stm32f407ve（板级描述/校验/装配/内存）
  │    └─ src/platform/stm32f407（驱动、映射、时钟、boot、console、IRQ、panic）
  ├─ src/framework/components（protocol / display / task）
  └─ src/framework/core（设备注册表 + class）

src/framework/core/fw_dev.c → fw/dev.h（不认识 pin/serial/spi/display）
src/framework/port/rtthread → RT-Thread tick + 静态线程端口
src/vendor/rt-thread-nano → 生成的 rtconfig.h
```

依赖红线：

1. `src/framework/core` 与 `components` 不包含 HAL / RT-Thread 头文件；只有 `port/rtthread` 例外。
2. `core/fw_dev.c` 只保存路径、类型令牌、ops、drv；不认识具体设备类。
3. `src/platform/stm32f407` 实现驱动与通用启动；`src/platform/board` 只描述板级时钟、接线、内存和装配，驱动不直接读 `BOARD_*` 引脚。
4. 业务命令、启动画面和图片只在 `src/app`；framework/board 不引用业务资源。
5. `fw_task` 组件不认识 `rt_thread`；线程操作只通过 port ops。
6. ISR 只碰 ring/事件和 CPU tick 计数；不在 ISR 调用阻塞 API。
7. 默认静态分配，`RT_USING_HEAP=n`。

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
| `drv_pin.c` + `stm32f407_pin_map.h` | 同名/同结构 | 端口/引脚/IRQ 表 |
| `MR_INIT_*_EXPORT` | 显式 board 注册 | 避免链接段差异，启动顺序更直观 |
| `mr_malloc` ring | 静态 ring | 无堆、RAM 可预测 |

不直接 vendor mr-library 的原因：它的 ring/pin IRQ 依赖 malloc，`mr_config.h`
会成为第二配置源，且缺少本项目需要的 wait_rx 语义。

## 2.1 任务管理与健康度

```text
main（优先级 10，栈 4096）
  app_tasks_init：manager → adopt main → create rpc（不启动）
  app_rpc_init / app_tasks_start
  loop: fw_task_refresh → fw_task_next_wait_ms → mdelay

rpc（优先级 9，栈 APP_RPC_TASK_STACK_SIZE=2048）
  loop: fw_task_heartbeat_self → app_rpc_poll → wait_rx（上限 500ms）

idle（优先级 31）
  只计入 CPU idle 比例
```

- `fw_task_manager_t` 用 `FW_TASK_MAX=8` 的静态数组保存任务，不调用 malloc。
- `fw_task_refresh()` 由 main 周期调用：扫描栈 `'#'` 高水位、读取心跳年龄、计算 CPU 窗口。
- `fw_task_cpu_tick()` 由 SysTick 在 `rt_tick_increase` 前调用，按当前线程/idle/other 归桶。
- 健康等级：`OK < WARN < LOST`；reason 位：栈低、心跳 warn、心跳 lost、线程关闭。
- 心跳阈值默认 warn 1500ms、lost 3000ms；RPC 任务等待上限 500ms，保证不会心跳饥饿。
- 挂起采用协作式：control 置 `suspend_pending`，目标任务在下一次 `heartbeat_self` 自行
  `rt_thread_suspend + rt_schedule`。长期阻塞且不 heartbeat 的任务无法协作挂起。
- `main` 的 `controllable=false`；resume 只接受 `user_suspended` 的任务，避免误恢复事件阻塞线程。
- RPC：`0x05` 摘要、`0x06` 按索引详情、`0x07` 控制；异常只标记/上报，不自动重启。
- 栈高水位是近似值；SysTick 采样把 ISR 时间归到被打断线程，idle 可能略高。

## 3. 设备类

| 类 | 设备名 | 关键语义 |
|---|---|---|
| pin | `pin` | offset 是引脚号；`FW_CTL_PIN_SET_MODE` 配置模式 |
| serial | `serial1..N` | RX/TX ring；`GET/SET_CONFIG`、`CLR_*`、`GET_*DATASZ` |
| spi | `spi1..N` | `SET/GET_CONFIG`，transfer 由 class helper 调用 |
| display | `lcd` | 窗口/像素/背光；通用裁剪由 core 完成 |

serial 的 ring 由 driver/board 提供静态存储；driver 通过 `ops->lock/unlock`
注入 RT-Thread 临界区，core 不直接依赖 RTOS。

## 4. Platform / board

```text
src/platform/stm32f407/               芯片与通用平台服务
  drv_pin / drv_serial / drv_spi
  stm32f407_map / stm32f407_pin_map
  stm32f407_clock     数据驱动时钟
  stm32f407_boot      HAL/NVIC/SysTick/heap/复位原因
  stm32f407_irq       SysTick_Handler
  stm32f407_console   rt_hw_console_output + bind
  stm32f407_panic
  Kconfig.drivers     FW_USING_UARTn / FW_USING_SPIn

src/platform/board/stm32f407ve/        单板描述与装配
  Kconfig.board       时钟/板卡信息/内存/接线
  board_pins          串口/SPI 描述符 + 端口/AF/SWD/重复引脚校验
  board_clock         HSI/HSE clock_cfg
  board_devices       设备注册 + 控制台绑定 + LED/LCD
  board.c             薄 rt_hw_board_init + board info/reset
  link.lds            INCLUDE 生成的 board_memory.ld
```

- `drv_serial.c`/`drv_spi.c` 接收 `drv_serial_hw_t`/`drv_spi_hw_t`，不再包含 `board_config.h`。
- `stm32f407_clock.c` 接收 `stm32f407_clock_cfg_t`，板级只提供数据。
- `stm32f407_boot_init` 负责通用启动；`board.c` 只决定顺序和板级设备。
- `board_pins_validate()` 在注册前拒绝非法端口/AF/SWD/重复引脚。
- 启动画面由 `src/app/boot/app_boot.c` 通过 `board_lcd()` 获取设备后绘制。
- 唯一保留的编译期板级输入是 `stm32f4xx_hal_conf.h` 的 `BOARD_HSE_VALUE → HSE_VALUE`。

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
- 生成物包括 `rtconfig.h`、`board_config.h`、`fw_config.h`、`app_config.h`、
  `board_memory.ld`、`fw_config.cmake` 和 `config_summary.txt`。
- `board_memory.ld` 是 GCC 与 Keil scatter 的共同内存来源。
- `fwconfig.py check --all`、`check-layout`、`check-manifest`、`check-hygiene`、
  `gen-keil --check` 由 Host CTest 执行。

## 6. 启动与并发

```text
rt_hw_board_init
  stm32f407_boot_init(board_clock_config())
    HAL_Init → clock → SysTick → heap → reset cause
  board_devices_init
    board_pins_validate → pin/LED/serial/SPI/LCD 注册 → console bind
main
  fw_dev_open(console/pin) → board_serial_start_all
  app_boot_display(board_lcd())
  app_tasks_init（adopt main + create rpc）
  app_rpc_init(config.tasks=manager) → app_tasks_start
  监督循环：fw_task_refresh → fw_task_next_wait_ms → rt_thread_mdelay

rpc 任务
  fw_task_heartbeat_self → app_rpc_poll → fw_serial_wait_rx（≤500ms）
```

- SysTick：`rt_interrupt_enter → fw_task_cpu_tick → HAL_IncTick → rt_tick_increase → rt_interrupt_leave`。
- serial RX：RXNE ISR → class ring → `rt_event_send`；任务 wait/poll。
- serial TX：任务写 ring → 驱动 start_tx → TXE ISR 搬运。
- `fw_serial` 的 ring 访问通过 driver lock/unlock（RT-Thread PRIMASK）保护。
- LCD/SPI 只在 main 线程使用；首版不做多任务共享锁。

## 7. 验证现状

- Host CTest 6/6：单元测试、profile 一致性、布局检查、manifest、Keil 生成、卫生检查。
- F407 GCC：加入任务管理后默认配置构建通过（约 91.5 KB Flash / 14.9 KB RAM）。
- F407 HSE GCC：168 MHz 配置构建通过（RAM 相同）。
- Keil AC5：工程由 manifest 重新生成；本次重构后需要在 UV4 中重新编译复验。
- Kconfig-only：启用 UART2/SPI2、LED 改 PE3 的既有场景仍由同一 Kconfig 树驱动。
- 真实硬件 HIL 仍需上板执行。
