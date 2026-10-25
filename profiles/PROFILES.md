# profiles

每个 profile = 一份构建配置：

```text
profiles/<name>/defconfig     构建选择模板
profiles/<name>/.config       当前构建选择
profiles/<name>/generated/    rtconfig.h / fw_config.h / app_config.h /
                              board_policy.h / fw_config.cmake / config_summary.txt
```

板级接线（身份、内存、引脚/AF、通道、LCD、NVM）只在
`src/platform/board/stm32f407ve/board_config.h` 手写；profile 不复制
引脚/AF/内存，也没有 `profile.json`。

| profile | 用途 |
|---|---|
| `host` | Host 单元测试（无 RTOS/HAL，无板级策略） |
| `f407ve` | STM32F407VE 默认构建（HSI 16 MHz），也被 Keil AC5 使用 |
| `f407ve-hse` | HSE 8 MHz → 168 MHz 变体 |
| `f407ve-dma` | HSI + UART1 RX/TX DMA + SPI1 TX DMA + pin EXTI，用于上板验证 DMA/EXTI |

CMake 通过 `FW_PROFILE` 选择 profile；Keil 按 profile 生成独立工程
（`stm32f407ve` / `stm32f407ve-hse`）。两者不再各自维护一份 `.config`。

## 常用命令

```text
python tools/fwconfig.py sync --all
python tools/fwconfig.py menuconfig --profile f407ve
python tools/fwconfig.py check --all
python tools/fwconfig.py check-layout
python tools/fwconfig.py check-manifest
python tools/fwconfig.py check-hygiene
python tools/fwconfig.py gen-keil --profile f407ve
python tools/fwconfig.py gen-keil --profile f407ve-hse
```

默认 profile 的 UART 仍走 IT 路径、SPI 仍走轮询；三个目标 profile 都启用
`FW_USING_I2C1`、`FW_USING_ADC1`、`FW_USING_PWM4`，用于验证新增的阻塞/
轮询类。I2C1 使用 PB6/PB7，ADC1 使用 PB0/PB1，PWM4 使用 PD12..PD15。
所有目标 profile 的板级尾部 256KB 作为 NVM 参数区，代码区为 256KB。

`f407ve-dma` 另外打开 `FW_USING_UART1_DMA_RX/TX`、`FW_USING_SPI1_DMA`、
`FW_USING_PIN_EXTI` 与 `BOARD_IWDG_ENABLE`，用于验证 DMA/EXTI/看门狗代码路径。
SPI1 的 MISO 关闭，因此只分配 TX DMA（DMA2 Stream3 Channel3）；
全双工验证需要启用 SPI2/SPI3。IWDG 启动后不可停止，调试时可临时关闭。
它没有独立的 Keil 工程；需要时用 CMake preset：

```text
cmake --preset f407-dma-debug
cmake --build --preset f407-dma-debug
```

`sync` 在 `.config` 不存在时才从 `defconfig` 复制；之后生成
`rtconfig.h`、`board_policy.h`、`fw_config.h`、`app_config.h`、
`fw_config.cmake` 和 `config_summary.txt`，不覆盖用户配置。
旧的 `board_config.h`/`board_desc.h`/`board_memory.ld`/`board_keil.json`
等生成物会被自动删除。

## 增加实例或改接线

- 已有 UART/SPI 实例：只改 Kconfig 的 `FW_USING_UARTn`/`FW_USING_SPIn`；
- 改引脚/AF、新增实例、改控制台/LED/LCD 绑定：编辑
  `src/platform/board/stm32f407ve/board_config.h`；
- 改时钟源、波特率、IRQ、频率、IWDG：改 `Kconfig.board`（或 menuconfig）；
- 然后 `sync --all`、`gen-keil`，重新构建 CMake/Keil。

不需要修改其他 C/H；`board.c` 按 Kconfig 启用项过滤 C 表并注册设备。
当前驱动只覆盖 UART1..3、SPI1..3；新增 UART4+ 需要扩展驱动实例槽位和
`stm32f407_map.c`。
