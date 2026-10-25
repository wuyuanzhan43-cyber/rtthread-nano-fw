# src：唯一代码根

所有固件与测试源码都在这里；根目录只保留配置、构建、打包与文档。
Keil 专用启动文件 `startup_stm32f407xx.s` 属于工具链资产，仍放在 `keil/`。

```text
src/
├─ app/                 业务 + 组合根
│  ├─ main.c            组合根 + 健康监督者
│  ├─ include/app/      rpc.h、boot.h、tasks.h
│  ├─ rpc/              PING/LED/STATUS/STATS/TASK、0x90 上报
│  ├─ task/             adopt main、创建/启动 rpc + worker 任务
│  ├─ boot/             启动画面
│  └─ assets/           头像等应用资源
├─ framework/           与芯片/OS 无关
│  ├─ include/fw/       公共 API
│  ├─ core/             fw_*.c 设备核心与类
│  ├─ components/       protocol / display / task
│  └─ port/rtthread/    RT-Thread tick + 静态线程适配
├─ platform/            芯片 + 板级
│  ├─ stm32f407/        驱动(UART/SPI/I2C/ADC/PWM/DMA/EXTI)、映射、时钟、boot、console、fault、IWDG、panic
│  └─ board/stm32f407ve/board.c 组合根、board_config.h 接线表、link.lds
├─ tests/host/          Host 假设备/假任务、framework/task/RPC/peripheral 单测
├─ tests/hil/           串口协议 HIL（含 --stress）
├─ third_party/         STM32F4 HAL/CMSIS/device 子集
└─ vendor/              官方 RT-Thread Nano 3.1.5
```

仓库根只保留 profile 构建策略：

```text
profiles/<profile>/defconfig /.config  启用项/容量/栈/IRQ/时钟源
profiles/<profile>/generated/          board_policy.h 等策略头
```

依赖方向：

```text
app/main ──→ app/task、app/rpc、app/boot、platform/board
app/task ──→ fw_task + rtthread_task 端口
app/rpc  ──→ framework（dev/serial/pin/clock/protocol/task）
platform/stm32f407/drv_dma ──→ HAL DMA；drv_serial/drv_spi/drv_pin 复用它的 stream/IRQ 底座
platform/stm32f407/drv_spi ──→ HAL SPI 轮询 / DMA + rt_semaphore（异步完成）
platform/stm32f407/drv_i2c ──→ HAL I2C 主模式阻塞（mem_read 使用 repeated start）
platform/stm32f407/drv_adc ──→ HAL ADC 单通道轮询
platform/stm32f407/drv_pwm ──→ HAL TIM4 PWM（四通道）
platform/stm32f407/stm32f407_nvm ──→ fw_param 的 Flash 后端
platform/stm32f407/stm32f407_fault / stm32f407_iwdg ──→ Fault 现场与看门狗
framework/core/fw_log、fw_param ──→ 诊断与持久化（纯 C）
app/boot ──→ framework、app/assets
platform/board ──→ platform/stm32f407 ──→ framework
framework/port/rtthread ──→ RT-Thread
framework/core、components ──→ 纯 C，不含 HAL / RT-Thread
tools/gen_keil.py ──→ project/sources.json + board_config.h + board_policy.h
```

约定：

- 源文件清单是 `project/sources.json`；新建 `.c` 后要登记，否则
  `fwconfig.py check-manifest` 会报错。
- `#include` 使用 include 目录相对路径（如 `"fw/dev.h"`、`"app/rpc.h"`），
  不写 `src/` 前缀。
- 业务命令、启动画面和图片只能放在 `app/`；framework 与 platform 不出现业务资源。
- `third_party/` 与 `vendor/` 只做最小补丁，不在这里加业务代码。
- 第一方 `.c/.h/Kconfig*` 文件名必须唯一；`fwconfig.py check-hygiene` 会检查。
