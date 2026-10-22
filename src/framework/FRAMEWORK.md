# framework

| 目录 | 职责 | 依赖 |
|---|---|---|
| `include/fw/dev.h` | mr 风格设备核心：路径、ops、drv、open/close/read/write/ioctl/isr | 标准 C |
| `include/fw/{pin,serial,spi,display}.h` | 类型化设备类 | `fw/dev.h` |
| `include/fw/{protocol,st7735s}.h` | 帧协议与面板驱动的公共 API | 标准 C / 设备类 |
| `include/fw/task.h` | OS 中立的任务管理、健康度、控制 API | 标准 C |
| `include/fw/rtthread_clock.h` | RT-Thread 毫秒时钟的公共 API | `fw/clock.h` |
| `include/fw/rtthread_task.h` | RT-Thread 静态线程端口声明 | `fw/task.h` |
| `core/fw_dev.c` | 静态注册表、路径归一化、引用计数、ops 分发 | `fw/dev.h` |
| `core/fw_{pin,serial,spi,display}.c` | 设备类包装；serial 有静态 ring + lock/unlock 注入 | `fw/dev.h` |
| `components/protocol/fw_protocol.c` | A5 5A 帧、CRC-16/CCITT-FALSE | `fw/status.h` |
| `components/display/st7735s.c` | ST7735S，使用 `spiN` + `pin` 设备 | `fw/display.h`、`fw/spi.h`、`fw/pin.h` |
| `components/task/fw_task.c` | 静态任务表、心跳/栈/CPU 健康度、控制 | `fw/task.h` |
| `core/fw_serial.c`（DMA 辅助） | `rx_push`/`tx_peek`/`tx_consume`，ring 所有权留在类层 | `fw/serial.h` |
| `core/fw_pin.c`（EXTI 类） | attach/detach/enable、静态回调表、`isr` 分派 | `fw/pin.h` |
| `port/rtthread/rtthread_clock.c` | RT-Thread tick → `fw_clock_t` | RT-Thread kernel |
| `port/rtthread/rtthread_task.c` | 静态 rt_thread、状态/栈快照、挂起/恢复/优先级 | `fw/rtthread_task.h`、RT-Thread |

## 红线

1. 不在这里包含 HAL/RT-Thread 头文件（`port/rtthread` 除外）。
2. `core/fw_dev.c` 不认识 pin/serial/spi/display 的具体类型；类型令牌由各 class 模块定义。
3. `components/task/fw_task.c` 不认识 `rt_thread`；线程操作只通过 `fw_task_port_ops_t`。
4. 业务命令、启动画面、图片资源只能放在 `src/app/`；framework 不出现 PING/LED/STATUS/STATS、0x90 或头像。
5. 业务只通过 `fw_dev_*` 或 class helper 访问设备。
6. 所有内存静态分配；serial ring、任务栈和任务表由 driver/board/app 静态提供，不使用 `malloc`。

## 设备名

| 设备 | 语义 |
|---|---|
| `pin` | 一个设备覆盖所有 GPIO；offset = `(port << 4) | pin` |
| `serial1..N` | 每个启用的 UART 一个设备 |
| `spi1..N` | 每个启用的 SPI 一个设备 |
| `lcd` | ST7735S 显示设备 |

路径查找支持 `fw_dev_find("serial1")` 和 `fw_dev_find("/dev/serial1")`；当前是静态扁平注册表，不引入动态设备树。

## 任务管理

| API | 语义 |
|---|---|
| `fw_task_manager_init` | 绑定 port ops、clock、刷新周期 |
| `fw_task_create/adopt/start` | 静态创建或接管线程 |
| `fw_task_heartbeat/self` | 更新心跳；self 同时处理协作挂起 |
| `fw_task_refresh` | 刷新状态/栈/心跳/CPU 窗口 |
| `fw_task_summary/get_info` | 摘要与按索引详情 |
| `fw_task_control` | suspend pending、resume、set_priority |
| `fw_task_cpu_tick` | SysTick 中按当前/idle/other 归桶 |

健康度只做标记：`STACK_LOW`、`HB_WARN`、`HB_LOST`、`CLOSED`；
不自动重启、不自动恢复。栈高水位是近似值，CPU 为 1ms 采样。

## 扩展

新增设备类型（例如 ADC）：

```text
include/fw/adc.h                 类型令牌、ops、fw_adc_t
core/fw_adc.c                    包装 + class 逻辑
src/platform/stm32f407/drv_adc.c 芯片驱动
boards/<board>/board.json        实例/引脚/通道绑定
chips/<chip>.json                AF/通道映射
project/sources.json             登记源文件
```

`core/fw_dev.c` 不需要修改。

新增/修改 UART/SPI/I2C 等实例接线改 board manifest；已有实例的启用仍改
`FW_USING_UARTn`/`FW_USING_SPIn`。生成器会校验 function+AF、重复引脚、
DMA 流冲突和 EXTI 线冲突，不需要手写 AF 表。

## DMA / EXTI

- UART DMA 由 `FW_USING_UARTn_DMA_RX/TX` 选择；驱动用 `fw_serial_rx_push`
  把 DMA 块写入类 ring，用 `tx_peek/tx_consume` 做 staging 发送。
- `fw_serial_ops_t` 末尾的 `flush` 用于 CLR_WR_BUF 前 abort TX DMA。
- pin EXTI 由 `FW_USING_PIN_EXTI` 选择；回调表在 `fw_pin_t` 内静态分配，
  驱动 ISR 通过 `fw_dev_isr(FW_ISR_PIN_EDGE)` 上报引脚号。
- 回调在 ISR 上下文执行，禁止阻塞；需要任务处理时在回调里发事件/信号量。
