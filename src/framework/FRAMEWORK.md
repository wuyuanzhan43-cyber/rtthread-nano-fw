# framework

| 目录 | 职责 | 依赖 |
|---|---|---|
| `include/fw/dev.h` | mr 风格设备核心：路径、ops、drv、open/close/read/write/ioctl/isr | 标准 C |
| `include/fw/{pin,serial,spi,i2c,adc,pwm,display}.h` | 类型化设备类 | `fw/dev.h` |
| `include/fw/{protocol,st7735s}.h` | 帧协议与面板驱动的公共 API | 标准 C / 设备类 |
| `include/fw/log.h` | 分级日志、sink、编译期裁剪 | 标准 C |
| `include/fw/param.h` | 双槽 A/B 参数存储 API | `fw/status.h` |
| `include/fw/task.h` | OS 中立的任务管理、健康度、控制 API | 标准 C |
| `include/fw/rtthread_clock.h` | RT-Thread 毫秒时钟的公共 API | `fw/clock.h` |
| `include/fw/rtthread_task.h` | RT-Thread 静态线程端口声明 | `fw/task.h` |
| `core/fw_dev.c` | 静态注册表、路径归一化、引用计数、ops 分发 | `fw/dev.h` |
| `core/fw_{pin,serial,spi,i2c,adc,pwm,display}.c` | 设备类包装；serial 有静态 ring + lock/unlock 注入 | `fw/dev.h` |
| `core/fw_log.c` | 固定行缓冲、级别过滤、sink 分发 | `fw/log.h` |
| `core/fw_param.c` | 槽头/CRC32/序号、A/B 选择与掉电回退 | `fw/param.h` |
| `port/rtthread/rtthread_log.c` | fw_log sink → `rt_hw_console_output` | RT-Thread、`fw/log.h` |
| `components/protocol/fw_protocol.c` | A5 5A 帧、CRC-16/CCITT-FALSE | `fw/status.h` |
| `components/display/st7735s.c` | ST7735S，使用 `spiN` + `pin` 设备 | `fw/display.h`、`fw/spi.h`、`fw/pin.h` |
| `components/task/fw_task.c` | 静态任务表、心跳/栈/CPU 健康度、控制 | `fw/task.h` |
| `core/fw_serial.c`（DMA 辅助） | `rx_push`/`tx_peek`/`tx_consume`，ring 所有权留在类层 | `fw/serial.h` |
| `core/fw_pin.c`（EXTI 类） | attach/detach/enable、静态回调表、`isr` 分派 | `fw/pin.h` |
| `port/rtthread/rtthread_clock.c` | RT-Thread tick → `fw_clock_t` | RT-Thread kernel |
| `port/rtthread/rtthread_task.c` | 静态 rt_thread、状态/栈快照、挂起/恢复/优先级 | `fw/rtthread_task.h`、RT-Thread |

## 红线

1. 不在这里包含 HAL/RT-Thread 头文件（`port/rtthread` 除外）。
2. `core/fw_dev.c` 不认识具体设备类；类型令牌集中定义在 `fw/dev.h`，各 class 只校验自己的令牌。
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
| `i2c1..3` | 每个 I2C 控制器一个设备；off=7 位从机地址 |
| `adc1` | ADC 控制器；off=通道号 |
| `pwm4` | TIM4；off=通道 0..3 |
| `lcd` | ST7735S 显示设备 |

路径查找支持 `fw_dev_find("serial1")` 和 `fw_dev_find("/dev/serial1")`；当前是静态扁平注册表，不引入动态设备树。

## SPI 同步与异步

| API | 语义 |
|---|---|
| `fw_spi_transfer` | 阻塞传输；有异步在途时返回 `FW_EBUSY` |
| `fw_spi_transfer_async` | 启动后立即返回 length；busy 置位 |
| `fw_spi_transfer_wait` | 等待完成；`0` 只轮询并返回 `FW_EAGAIN`；超时由类层 abort |
| `fw_spi_transfer_abort` | 取消在途传输；未忙时幂等成功 |

- async/wait/abort 在 ops 中必须“全有或全无”，注册期校验。
- TX/RX 缓冲和 CS 在 wait/abort 返回前必须保持有效；同一总线单 owner。
- 类层只维护 `busy`，不包含 RT-Thread；信号量和 HAL 回调在 platform 驱动。
- 单次长度上限为 `FW_SPI_MAX_TRANSFER`（65535，HAL Size 为 uint16_t）。

## I2C / ADC / PWM

| 类 | 主要 API | 语义 |
|---|---|---|
| I2C | `fw_i2c_transfer/mem_read/mem_write/probe` | 7 位地址；mem_read 用 repeated start；写后读是两个 STOP |
| ADC | `fw_adc_read/read_mv` | off=通道；单次转换；mV 四舍五入 |
| PWM | `fw_pwm_set_duty/start/stop/get_config` | 通道 0..3；千分比；启停为全局 |

- 三个类首版全部阻塞/轮询，不引入 ring、异步或 DMA。
- I2C 类层地址固定为 7 位，驱动左移成 HAL 的 8 位格式。
- ADC 一个设备覆盖所有已接线通道；`fw_dev_read` 要求缓冲区至少 2 字节。
- PWM 的 `start/stop` 对应整个定时器；初始化后输出保持停止。

## 日志与诊断

- `fw_log`：`FW_LOGE/W/I/D` 宏 + `fw_log_printf`；`FW_LOG_LEVEL` 编译期裁剪，
  运行期 `fw_log_set_level()` 只能降低。
- sink 由 port 绑定；RT-Thread 端口输出到 `rt_hw_console_output`。
- 行缓冲 128 字节、不可重入，只在任务上下文使用；fault/panic 使用
  platform 的直写控制台，不经过 fw_log。
- `fw_dev_t.error_count` 由类层在驱动返回负值时递增；
  `fw_dev_note_error()` / `fw_dev_error_count()` 是唯一入口。

## 参数存储

| API | 语义 |
|---|---|
| `fw_param_init` | 绑定后端，扫描两个槽并选择最新有效记录 |
| `fw_param_save` | 擦除非活动槽，写槽头+数据，回读后切换 |
| `fw_param_load` | 重新校验 CRC 后复制数据 |
| `fw_param_clear` | 擦除两个槽 |
| `fw_param_capacity/valid` | 查询容量/是否有有效记录 |

- 槽头 20 字节：magic/version/sequence/size/crc32。
- 后端只要求 read/program/erase；platform 的 STM32 后端使用两个独立扇区。
- `FW_PARAM_DATA_MAX + 20` 必须不超过 `slot_size`，生成期校验。
- 单实例、任务上下文；save 会擦除/编程 Flash，不能从 ISR 调用。

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

新增设备类型（例如 DAC）：

```text
include/fw/dev.h                 追加 FW_DEV_TYPE_xxx 令牌
include/fw/dac.h                 ops、配置、fw_dac_t
core/fw_dac.c                    包装 + class 逻辑
src/platform/stm32f407/drv_dac.c 芯片驱动
board/stm32f407ve/board_config.h 实例/引脚/通道绑定（C 表）
board/stm32f407ve/board.c        build_*_hw 转换与注册
project/sources.json             登记源文件
```

`core/fw_dev.c` 不需要修改；类型令牌集中在 `fw/dev.h`。

新增/修改 UART/SPI/I2C/ADC/PWM 实例接线改 `board_config.h`；已有实例的
启用仍改对应 `FW_USING_*`。启动前 `board_pins_validate()` 检查重复引脚、
通道唯一和 DMA 流冲突；Host 表测试覆盖结构，AF 正确性由数据手册和上板
验证保证。

## DMA / EXTI

- UART DMA 由 `FW_USING_UARTn_DMA_RX/TX` 选择；驱动用 `fw_serial_rx_push`
  把 DMA 块写入类 ring，用 `tx_peek/tx_consume` 做 staging 发送。
- `fw_serial_ops_t` 末尾的 `flush` 用于 CLR_WR_BUF 前 abort TX DMA。
- SPI DMA 由 `FW_USING_SPIn_DMA` 选择；TX 总是分配，RX 额外要求
  `BOARD_SPIn_USE_MISO=y`。同步传输 >=16 字节走 async+wait，短命令仍轮询。
- SPI 完成/错误回调只 release 一次信号量；HAL 的 TX/RX abort 可能重复回调，
  由驱动的 `active` 标志和启动前 DMA READY 检查兜底。
- I2C/ADC/PWM 首版不走 DMA 和 IRQ：I2C/ADC 使用 HAL 阻塞轮询，PWM 仅配置
  TIM4 输出；后续可沿用 serial/SPI 的 DMA 底座扩展。
- pin EXTI 由 `FW_USING_PIN_EXTI` 选择；回调表在 `fw_pin_t` 内静态分配，
  驱动 ISR 通过 `fw_dev_isr(FW_ISR_PIN_EDGE)` 上报引脚号。
- 回调在 ISR 上下文执行，禁止阻塞；需要任务处理时在回调里发事件/信号量。
