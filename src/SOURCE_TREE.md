# src：唯一代码根

所有固件与测试源码都在这里；根目录只保留配置、构建、打包与文档。
Keil 专用启动文件 `startup_stm32f407xx.s` 属于工具链资产，仍放在 `keil/`。

```text
src/
├─ app/                 业务 + 组合根
│  ├─ main.c            组合根 + 健康监督者
│  ├─ include/app/      rpc.h、boot.h、tasks.h
│  ├─ rpc/              PING/LED/STATUS/STATS/TASK、0x90 上报
│  ├─ task/             adopt main、创建/启动 rpc 任务
│  ├─ boot/             启动画面
│  └─ assets/           头像等应用资源
├─ framework/           与芯片/OS 无关
│  ├─ include/fw/       公共 API
│  ├─ core/             fw_*.c 设备核心与类
│  ├─ components/       protocol / display / task
│  └─ port/rtthread/    RT-Thread tick + 静态线程适配
├─ platform/            芯片 + 板级
│  ├─ stm32f407/        驱动、映射、时钟、boot、console、IRQ、panic
│  └─ board/stm32f407ve/板级时钟/接线描述、校验、装配、link.lds
├─ tests/host/          Host 假设备/假任务与单测
├─ tests/hil/           串口协议 HIL
├─ third_party/         STM32F4 HAL/CMSIS/device 子集
└─ vendor/              官方 RT-Thread Nano 3.1.5
```

依赖方向：

```text
app/main ──→ app/task、app/rpc、app/boot、platform/board
app/task ──→ fw_task + rtthread_task 端口
app/rpc  ──→ framework（dev/serial/pin/clock/protocol/task）
app/boot ──→ framework、app/assets
platform/board ──→ platform/stm32f407 ──→ framework
framework/port/rtthread ──→ RT-Thread
framework/core、components ──→ 纯 C，不含 HAL / RT-Thread
```

约定：

- 源文件清单是 `project/sources.json`；新建 `.c` 后要登记，否则
  `fwconfig.py check-manifest` 会报错。
- `#include` 使用 include 目录相对路径（如 `"fw/dev.h"`、`"app/rpc.h"`），
  不写 `src/` 前缀。
- 业务命令、启动画面和图片只能放在 `app/`；framework 与 platform 不出现业务资源。
- `third_party/` 与 `vendor/` 只做最小补丁，不在这里加业务代码。
- 第一方 `.c/.h/Kconfig*` 文件名必须唯一；`fwconfig.py check-hygiene` 会检查。
