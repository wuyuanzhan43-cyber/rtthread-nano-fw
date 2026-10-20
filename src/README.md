# src：唯一代码目录

所有固件与测试源码都在这里；根目录只保留配置、构建、打包与文档。Keil 专用
启动文件 `startup_stm32f407xx.s` 属于工具链资产，仍放在 `keil/`。

| 目录 | 职责 |
|---|---|
| `app/` | 组合根 `main.c` |
| `framework/` | 自建设备模型与组件，不依赖 HAL |
| `bsp/` | STM32F407 驱动层：pin/serial/spi 与 mr_board 引脚表 |
| `board/` | stm32f407ve 板级装配、时钟、console、链接脚本 |
| `tests/` | Host 单测与串口 HIL |
| `third_party/` | STM32F4 HAL/CMSIS/device 子集 |
| `vendor/` | 官方 RT-Thread Nano 3.1.5 |

依赖方向：

```text
app ──→ framework
board ──→ bsp ──→ framework
bsp ──→ 设备 class ops
```

约定：

- 源文件清单仍是 `project/sources.json`；在这里新建 `.c` 后要登记，否则
  `fwconfig.py check-manifest` 会报错。
- `#include` 使用 include 目录相对路径（如 `"fw/dev.h"`），不写 `src/` 前缀。
- `third_party/` 与 `vendor/` 只做最小补丁，不在这里加业务代码。
