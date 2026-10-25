# 架构说明（整改后）

## 1. 分层

```text
src/app/main.c（监督者）
  ├─ src/app/task（adopt main、创建 rpc + worker 任务）
  ├─ src/app/rpc（命令分发、上报）
  ├─ src/app/boot（启动画面）+ src/app/assets（头像）
  ├─ src/platform/board/stm32f407ve（板级描述/校验/装配/内存）
  │    └─ src/platform/stm32f407（驱动、映射、时钟、boot、console、IRQ、fault、IWDG、panic）
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
| spi | `spi1..N` | `SET/GET_CONFIG`；同步 transfer + async/wait/abort（busy 状态机） |
| i2c | `i2c1..3` | 主模式阻塞；off=7 位地址；transfer/mem_read/mem_write/probe |
| adc | `adc1` | off=通道；单次轮询采样；read/read_mv |
| pwm | `pwm4` | off=通道 0..3；频率+千分比占空比；全局 start/stop |
| display | `lcd` | 窗口/像素/背光；通用裁剪由 core 完成 |

serial 的 ring 由 driver/board 提供静态存储；driver 通过 `ops->lock/unlock`
注入 RT-Thread 临界区，core 不直接依赖 RTOS。I2C/ADC/PWM 首版没有 ring、
中断或 DMA，全部为阻塞/轮询调用。

## 3.1 DMA 与 EXTI 底座

```text
drv_dma.c
  owner[16] = (controller,stream) -> DMA_HandleTypeDef*
  16 个 DMAx_Streamy_IRQHandler -> HAL_DMA_IRQHandler
  claim/release + CCM 地址检查

drv_serial.c（UART DMA）
  RX: circular 2*CHUNK -> 半传输/全传输 -> fw_serial_rx_push -> ring
  TX: wr_fifo -> fw_serial_tx_peek -> staging -> HAL_UART_Transmit_DMA
      完成回调 -> fw_serial_tx_consume -> 下一块
  默认 IT 路径保持不变；DMA 由 FW_USING_UARTn_DMA_RX/TX 选择

drv_spi.c（SPI DMA）
  async: caller buffer -> HAL_SPI_{Transmit,Receive,TransmitReceive}_DMA
  wait:  rt_semaphore；完成/错误回调只 release 一次
  abort: HAL_SPI_Abort -> 状态/CR2 兜底 -> drain -> 检查 DMA READY
  同步 transfer：DMA 开且 length>=16 走 async+wait，否则 HAL 轮询

drv_pin.c（EXTI）
  SYSCFG/EXTI 配置 + 7 个 EXTI IRQ 入口
  fw_dev_isr -> pin 类回调表 -> 应用回调（ISR 上下文）
```

- DMA 缓冲全部静态放在主 SRAM，禁止 CCM；`drv_dma_ptr_ok` 运行期兜底。
- stream/channel 在 `board_config.h` 的 C 表中手写；启动期检查合法性
  与同一 stream 冲突。
- `fw_serial` 保留 ring 所有权，DMA 驱动只通过 rx_push/tx_peek/tx_consume
  访问；这样 DMA 与 IT 路径共享同一套读/写/溢出语义。
- SPI 没有 ring：异步期间直接使用调用者缓冲，因此 TX/RX 在 wait/abort
  返回前必须保持有效，CS 也必须保持有效。
- SPI DMA 的完成信号量只在 platform 驱动内使用；`fw_spi` 只维护 busy，
  不包含 RT-Thread 头文件。
- RX DMA 需要 2LINES（`BOARD_SPIn_USE_MISO=y`）且 TX 方向同时存在；
  SPI1 的 MISO 与 LCD DC 同为 PA6，因此默认 SPI1 只能 TX-only。
- 默认 profile 不启用 DMA/EXTI；`f407ve-dma` 打开 UART1 DMA、SPI1 TX DMA
  与 pin EXTI，用于上板验证。

## 3.2 可靠性底座

```text
stm32f407_fault.c
  HardFault -> RT-Thread context_gcc/rvds 入口 -> rt_exception_hook
  MemManage/BusFault/UsageFault -> 强符号 naked 入口
  保存 R0-R3/R12/LR/PC/xPSR + CFSR/HFSR/MMFAR/BFAR
  stm32f407_console_panic_write -> 绕过 ring 直接轮询 USART

stm32f407_iwdg.c
  HAL_IWDG + LSI 32kHz 估算预分频/重装
  main 只在 fw_task_summary().lost == 0 时喂狗

fw_log.c + port/rtthread/rtthread_log.c
  编译期 FW_LOG_LEVEL 裁剪；运行期级别可降
  sink 绑定到 rt_hw_console_output
```

- Fault 钩子不返回：按 `BOARD_FAULT_RESET` 选择 `NVIC_SystemReset` 或停机。
- IWDG 一旦启动不可停止；默认关闭，`f407ve-dma` 打开用于验证。
- panic 输出 file:line；fault 输出向量名、栈帧和 SCB 故障寄存器。
- `fw_dev_t.error_count` 由各设备类在驱动返回负值时累加；
  计数非原子，沿用单 owner 约定。
- 日志行缓冲固定 128 字节，不可重入，只在任务上下文使用。

## 3.3 NVM / 参数存储

```text
src/platform/board/stm32f407ve/board_config.h
  BOARD_NVM_ORIGIN=0x08040000
  BOARD_NVM_SIZE_KB=256
  BOARD_NVM_SECTOR_KB=128
  BOARD_NVM_SLOT_KB = SIZE_KB / 2

board.c / link.lds / gen_keil.py
  FLASH 代码区 = 512K - 256K = 256K（link.lds / Keil IROM / scatter 一致）
  NVM 区域 0x08040000..0x0807FFFF，不参与固件链接

fw_param.c
  两个 slot，各一个 128KB 扇区
  槽头: magic/version/sequence/size/crc32
  save: 擦除非活动槽 -> 写槽头 -> 写数据 -> 回读校验 -> 切换
  init/load: 选择序号最高且 CRC 有效的槽

stm32f407_nvm.c
  HAL_FLASHEx_Erase / HAL_FLASH_Program 32-bit word
  地址 -> F407 扇区映射；擦除后回读校验
```

- 掉电发生在擦除/编程中途时，旧槽仍完整；下一次 init 回退。
- 每次 save 擦除非活动槽，适合低频参数（校准、设备配置）。
- `FW_PARAM_DATA_MAX` 必须小于 `slot_size - 20`，生成期校验。

## 4. Platform / board

```text
src/platform/stm32f407/               芯片与通用平台服务
  drv_pin / drv_serial / drv_spi / drv_i2c / drv_adc / drv_pwm
  stm32f407_map / stm32f407_pin_map
  stm32f407_clock     数据驱动时钟
  stm32f407_boot      HAL/NVIC/SysTick/heap/复位原因
  stm32f407_irq       SysTick_Handler
  stm32f407_console   rt_hw_console_output + bind + panic 直写
  stm32f407_fault     HardFault 钩子 + MemManage/BusFault/UsageFault
  stm32f407_iwdg      IWDG 启动/喂狗
  stm32f407_nvm       参数存储 Flash 后端
  stm32f407_panic     file:line + 直写 + 可选复位
  Kconfig.drivers     FW_USING_UARTn/SPIn/I2Cn/ADC1/PWM4

src/platform/board/stm32f407ve/        单板（mr-library 风格单文件组合根）
  board_config.h      纯 C 接线事实源：身份/内存/引脚/通道/DMA/LCD/NVM
  board.c             组合根：C 表 -> drv_*_hw_t、注册、时钟、校验
  board.h             应用可见的板级 API
  Kconfig.board       只保留策略（时钟源/启用项/波特率/IRQ/频率/IWDG）
  link.lds            内联 FLASH 代码区/NVM/RAM/CCM/最小栈
```

- `board_config.h` 只依赖 `stdbool.h`/`stdint.h` 和生成的 `board_policy.h`，
  Host 测试可直接包含并验证表结构。
- `board.c` 用 `build_serial_hw`/`build_spi_hw`/... 把 C 表按 `FW_USING_*`
  过滤后转换成驱动描述符；不再有 `board_pins`/`board_devices`/
  `board_peripherals`/`board_display`/`board_storage`/`board_clock`。
- `drv_serial.c`/`drv_spi.c` 接收 `drv_serial_hw_t`/`drv_spi_hw_t`，不直接读引脚宏；
  `drv_spi.c` 只额外使用 `BOARD_DMA_IRQ_PRIORITY` 设置 SPI 错误中断优先级。
- `stm32f407_clock.c` 接收 `stm32f407_clock_cfg_t`；`board.c` 内 `s_clock`
  按 `BOARD_CLOCK_HSE_8_168` 选择 HSI 16 MHz 或 HSE 168 MHz。
- `stm32f407_boot_init` 负责通用启动；`board.c` 只决定顺序和板级设备。
- `board_pins_validate()` 在注册前拒绝非法端口/保留脚/重复引脚，
  以及 enabled 但路由非法/缺 MISO 的 SPI DMA 描述符；AF 只做范围与
  同实例一致性检查，正确性由 Host 表测试和上板验证保证。
- 启动画面由 `src/app/boot/app_boot.c` 通过 `board_lcd()` 获取设备后绘制。
- `stm32f4xx_hal_conf.h` 继续通过 `BOARD_HSE_VALUE` 得到 `HSE_VALUE`。

## 5. 配置

```text
src/platform/board/stm32f407ve/board_config.h   引脚/AF/通道/内存/NVM 唯一来源
profiles/<profile>/defconfig
profiles/<profile>/.config
profiles/<profile>/generated/                   rtconfig.h / board_policy.h 等
```

- 板级接线是手写 C 表；Kconfig 只保留构建策略，生成 `board_policy.h`。
- `fwconfig.py sync`：加载 Kconfig profile，生成策略头、CMake 片段和摘要；
  Host profile 的 `board_policy.h` 为空，让 `board_config.h` 的默认值生效。
- CMake 的 `FW_PROFILE` 和 Keil 都使用同一个 profile；没有 per-build/Keil 配置。
- `project/sources.json` 是 CMake 与 Keil 的唯一源文件清单。
- `tools/gen_keil.py` 从 source manifest + `board_config.h` + `board_policy.h`
  + 模板生成工程；IROM 使用代码区 256K。
- 生成物包括 `rtconfig.h`、`board_policy.h`、`fw_config.h`、`app_config.h`、
  `fw_config.cmake` 和 `config_summary.txt`。
- 旧的 `board_config.h`/`board_desc.h`/`board_memory.ld`/`board_keil.json`/
  `board_summary.txt`/`board_pinout.md`/`board_channels.md` 由 sync 自动删除，
  `check-layout` 会拒绝回归。
- `fwconfig.py check --all`、`check-layout`、`check-manifest`、
  `check-hygiene`、`gen-keil --check` 和 `tools/tests/test_static_board.py`
  由 Host CTest 执行。

## 6. 启动与并发

```text
rt_hw_board_init
  stm32f407_boot_init(&s_clock)
    HAL_Init → clock → SysTick → heap → reset cause
  board_devices_init（顺序调用）
    system   : fault 钩子 + DMA owner 表
    pins     : board_pins_validate
    pin/LED  : drv_pin + LED 引脚
    serial   : build_serial_hw → serialN + console bind
    spi/i2c/adc/pwm : build_*_hw → 注册
    storage  : NVM 后端 + fw_param_init（扫描 A/B 槽）
    display  : pin + spiN → "lcd"
main
  fw_rtthread_log_bind()
  fw_dev_open(console/pin) → board_serial_start_all
  app_boot_display(board_lcd())
  app_tasks_init（adopt main + create rpc + worker）
  app_rpc_init(config.tasks=manager) → app_tasks_start
  board_watchdog_start()
  监督循环：fw_task_refresh → summary.lost==0 时喂狗
            → fw_task_next_wait_ms → rt_thread_mdelay

rpc 任务
  fw_task_heartbeat_self → app_rpc_poll → fw_serial_wait_rx（≤500ms）
```

- SysTick：`rt_interrupt_enter → fw_task_cpu_tick → HAL_IncTick → rt_tick_increase → rt_interrupt_leave`。
- serial RX：RXNE ISR 或 DMA 半传输/全传输 → `fw_serial_rx_push` → class ring
  → `rt_event_send`；任务 wait/poll。
- serial TX：任务写 ring → 驱动 start_tx → TXE ISR 或 DMA staging；
  DMA 完成回调消费 ring 后继续下一块。
- SPI DMA：async 启动 → 任务 wait（信号量）→ 完成/错误回调置结果并
  release；超时由类层 abort，避免调用者误以为总线已空闲。
- I2C：阻塞主模式事务；`mem_read/mem_write` 走 HAL 的 repeated start，
  `transfer` 的写后读是两个 STOP。
- ADC：初始化后保持 ADC 使能；每次 read 重新配置通道并软件触发一次转换。
- PWM：初始化时配置四路但不启动；`fw_pwm_start/stop` 控制整个 TIM4，
  `fw_pwm_set_duty` 可在运行期更新 CCR。
- 日志：main 先绑定 `fw_log` sink；之后的诊断走 `[E]/[W]/[I]/[D]` 前缀。
- 看门狗：IWDG 在任务全部启动后开启；main 每轮用 `fw_task_summary` 检查
  lost 计数，0 才喂狗。
- Fault：HardFault 由 RT-Thread 入口转入异常钩子；其余 fault 由
  platform 强符号入口处理；两者都直接轮询控制台输出。
- pin EXTI：EXTI ISR → `fw_dev_isr(FW_ISR_PIN_EDGE)` → pin 回调表。
- `fw_serial` 的 ring 访问通过 driver lock/unlock（RT-Thread PRIMASK）保护。
- DMA 缓冲全部在主 SRAM；CCM 不可 DMA。
- LCD/SPI 只在 main 线程使用；首版不做多任务共享锁。

## 7. 验证现状

- Host CTest 8/8：Host 单元测试（含板级 C 表结构）、profile 一致性、
  布局/源文件/卫生、静态板级一致性单测（8 项）、HSI/HSE Keil 生成检查。
- F407 GCC：默认配置构建通过（141,808 B Flash / 18,120 B RAM，代码区 256KB）。
  I2C1/ADC1/PWM4 与 NVM 参数存储默认启用。
- F407 HSE GCC：168 MHz 配置构建通过（大小相同）。
- F407 DMA profile：UART1 RX/TX DMA + SPI1 TX DMA + pin EXTI + IWDG 构建通过
  （150,224 B Flash / 18,400 B RAM）；DMA/EXTI/看门狗时序仍需上板 HIL。
  SPI1 只分配了 TX route（DMA2 Stream3 Channel3），RX 因 MISO 关闭而保持 0xFF。
- I2C/ADC/PWM：阻塞/轮询路径已编译并通过 Host 类测试；电气行为仍需上板。
- 可靠性：fault 现场、panic 直写、IWDG、fw_log、设备错误计数已编译；
  Host 覆盖日志过滤与错误计数，Fault/IWDG 需要上板触发验证。
- Keil AC5：工程由 `board_config.h` + `board_policy.h` 生成；
  `gen-keil --check` 只证明同步，仍需在 UV4 中实际编译复验。
- 板级拓扑：新增/修改 UART/SPI/I2C/ADC/PWM 接线只改 `board_config.h`；
  已有实例启用仍改 Kconfig。
- 真实硬件 HIL 仍需上板执行。
