# profiles

每个 profile 是同一份 Kconfig 的一个完整配置：

```text
profiles/<name>/defconfig     模板
profiles/<name>/.config       当前配置
profiles/<name>/generated/    生成头文件（rtconfig.h / board_config.h / fw_config.h / app_config.h / board_memory.ld）
```

| profile | 用途 |
|---|---|
| `host` | Host 单元测试（无 RTOS/HAL） |
| `f407ve` | STM32F407VE 默认构建，也被 Keil AC5 使用 |
| `f407ve-hse` | HSE 8 MHz → 168 MHz 变体 |

CMake 通过 `FW_PROFILE` 选择 profile；Keil 的 include 路径指向
`profiles/f407ve/generated`。两者不再各自维护一份 `.config`。

## 常用命令

```text
python tools/fwconfig.py sync --profile f407ve
python tools/fwconfig.py menuconfig --profile f407ve
python tools/fwconfig.py check --all
python tools/fwconfig.py gen-keil --profile f407ve
```

`sync` 在 `.config` 不存在时才从 `defconfig` 复制；之后只生成头文件、
`board_memory.ld` 和 CMake 片段，不覆盖用户配置。

## 只改 Kconfig 增加实例

例如启用 UART2 + SPI2、把 LED 改到 PE3：

1. 修改 `profiles/f407ve/.config`（或运行 menuconfig）；
2. `fwconfig.py sync --profile f407ve`；
3. `fwconfig.py gen-keil --profile f407ve`；
4. CMake 和 Keil 重新构建。

不需要修改任何 C/H；`board_pins.c` 会按 `FW_USING_UARTn/SPIn` 生成描述符，
`drv_serial.c`/`drv_spi.c` 按描述符注册。当前驱动只覆盖 UART1..3、SPI1..3；
新增 UART4+ 需要扩展驱动实例槽位和 `stm32f407_map.c`。
