# profiles

每个 profile = 一个 manifest 选择 + 一份构建配置：

```text
profiles/<name>/profile.json  board/chip/clock variant 选择
profiles/<name>/defconfig     构建选择模板
profiles/<name>/.config       当前构建选择
profiles/<name>/generated/    生成头文件/内存/Keil 信息
```

板级拓扑在 `chips/`、`boards/` 的 manifest 中；profile 不复制引脚/AF/时钟/内存。

| profile | 用途 |
|---|---|
| `host` | Host 单元测试（无 RTOS/HAL） |
| `f407ve` | STM32F407VE 默认构建，也被 Keil AC5 使用 |
| `f407ve-hse` | HSE 8 MHz → 168 MHz 变体 |
| `f407ve-dma` | HSI + UART1 RX/TX DMA + pin EXTI，用于上板验证 DMA/EXTI |

CMake 通过 `FW_PROFILE` 选择 profile；Keil 按 profile 生成独立工程
（`stm32f407ve` / `stm32f407ve-hse`）。两者不再各自维护一份 `.config`。

## 常用命令

```text
python tools/fwconfig.py sync --all
python tools/fwconfig.py menuconfig --profile f407ve
python tools/fwconfig.py check --all
python tools/fwconfig.py check-boards
python tools/fwconfig.py gen-keil --profile f407ve
python tools/fwconfig.py gen-keil --profile f407ve-hse
```

默认 profile 的 UART 仍走 IT 路径；`f407ve-dma` 打开
`FW_USING_UART1_DMA_RX/TX` 与 `FW_USING_PIN_EXTI`，用于验证 DMA/EXTI
代码路径。它没有独立的 Keil 工程；需要时用 CMake preset：

```text
cmake --preset f407-dma-debug
cmake --build --preset f407-dma-debug
```

`sync` 在 `.config` 不存在时才从 `defconfig` 复制；之后生成头文件、
`board_desc.h`、`board_memory.ld`、`board_keil.json` 等，不覆盖用户配置。

## 增加实例或改接线

- 已有 UART/SPI 实例：只改 Kconfig 的 `FW_USING_UARTn`/`FW_USING_SPIn`；
- 改引脚/AF、新增实例、改控制台/LED/LCD 绑定：编辑
  `boards/stm32f407ve/board.json`；芯片能力变化编辑 `chips/stm32f407.json`；
- 然后 `sync --all`、`check-boards`、`gen-keil`，重新构建 CMake/Keil。

不需要修改 C/H；描述符由 `board_pins.c` 按 Kconfig 启用项组装。当前驱动只覆盖
UART1..3、SPI1..3；新增 UART4+ 需要扩展驱动实例槽位和 `stm32f407_map.c`。
