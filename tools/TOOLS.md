# 配置与工程生成工具

`kconfiglib.py` 与 `menuconfig.py` 已 vendor 到 `tools/kconfiglib/`；Windows
的交互式 menuconfig 需要 curses：

```text
python -m pip install -r tools/requirements.txt
```

## 配置源

```text
chips/<chip>.json                       芯片/封装/内存/AF/DMA/EXTI
boards/<board>/board.json               板卡身份/时钟/接线/设备绑定
profiles/<name>/profile.json            选择 board/chip/clock variant
profiles/<name>/{defconfig,.config}     构建选择（启用项/容量/栈/IRQ）
profiles/<name>/generated/              生成物
```

manifest 是板级拓扑的唯一来源；Kconfig 不再保存引脚/AF/时钟/内存。
CMake 与 Keil AC5 都消费 `profiles/<profile>/generated/`；没有
`build/<preset>/.config` 或 `keil/.config`。

Windows 下也可直接双击 `tools\menuconfig.cmd`，或带 profile 名称运行：

```text
tools\menuconfig.cmd
tools\menuconfig.cmd host
```

```text
python tools/fwconfig.py sync --profile f407ve
python tools/fwconfig.py menuconfig --profile f407ve
python tools/fwconfig.py check --all
python tools/fwconfig.py check-boards
python tools/fwconfig.py check-layout
python tools/fwconfig.py check-manifest
python tools/fwconfig.py check-hygiene
python tools/fwconfig.py gen-keil --profile f407ve
python tools/fwconfig.py gen-keil --profile f407ve-hse
```

- `sync`：`.config` 不存在时从 `defconfig` 复制，再生成 `generated/`。
- `menuconfig`：编辑 profile 的 `.config`，退出后自动 sync。
- `check`：验证 generated 与 `.config` 一致。
- `check-boards`：校验 chip/board manifest 与 Kconfig 的组合（引脚/AF/DMA/时钟/IRQ）。
- `check-layout`：拒绝旧的配置路径和重构前的源码目录。
- `check-manifest`：检查 `project/sources.json` 路径、重复和项目源文件覆盖。
- `check-hygiene`：拒绝第一方同名文件、示例式命名和旧 API 名字。
- `gen-keil`：从 source manifest + `board_keil.json` + 模板生成 uvprojx；
  按 profile 输出 `keil/stm32f407ve*.uvprojx/.sct`；`--check` 只验证不写。
- `tools/boardgen.py`：manifest 校验与 `board_desc.h`/`board_memory.ld`/
  `board_summary.txt`/`board_keil.json` 生成；被 genconfig 调用。
- `tools/tests/`：boardgen 无硬件单元测试，由 CTest `v2_tools_tests` 执行。

## 生成文件

| 文件 | 用途 |
|---|---|
| `rtconfig.h` | RT-Thread 内核 |
| `board_config.h` | 兼容 `BOARD_*` 宏 + manifest hash |
| `board_desc.h` | 板卡身份/时钟/命名引脚/DMA 路由 |
| `fw_config.h` | 框架组件与实例开关 |
| `app_config.h` | 应用层启动画面等配置 |
| `board_memory.ld` | 代码 Flash/NVM/RAM/CCM/最小栈 |
| `board_keil.json` | Keil Device/Cpu/FlashDll/IRAM/IROM/CLOCK |
| `board_summary.txt` | 引脚/DMA/时钟/内存人读摘要 |
| `fw_config.cmake` | CMake 侧 `CONFIG_*` |
| `config_summary.txt` | 人工检查摘要 |

## 源文件清单

`project/sources.json` 是 CMake 与 Keil 共用的唯一源文件清单。新增/删除源文件只改
这一处；`check-manifest` 会检查项目目录下的 `.c` 是否都已登记。

## 命名约定

- Kconfig 符号：`RT_*`、`BOARD_*`、`FW_*`
- `.config` / CMake：`CONFIG_*`
- C 头文件：去掉 `CONFIG_` 前缀
- `RT_*` 的 false 布尔不定义（RT-Thread 用 `#ifdef`）
- `BOARD_*` / `FW_*` 的 false 布尔显式定义为 0（业务用 `#if`）
