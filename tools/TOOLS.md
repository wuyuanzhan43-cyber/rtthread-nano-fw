# 配置与工程生成工具

`kconfiglib.py` 与 `menuconfig.py` 已 vendor 到 `tools/kconfiglib/`；Windows
的交互式 menuconfig 需要 curses：

```text
python -m pip install -r tools/requirements.txt
```

## 配置源

```text
src/platform/board/stm32f407ve/board_config.h   引脚/AF/通道/内存/NVM 唯一来源
profiles/<name>/{defconfig,.config}             构建策略（启用项/容量/栈/IRQ/时钟源）
profiles/<name>/generated/                      策略头与 CMake 片段
```

板级接线是手写 C 表；Kconfig 不再保存引脚/AF/时钟/内存。
CMake 与 Keil AC5 都消费 `profiles/<profile>/generated/`；没有
`profile.json`、`build/<preset>/.config` 或 `keil/.config`。

Windows 下也可直接双击 `tools\menuconfig.cmd`，或带 profile 名称运行：

```text
tools\menuconfig.cmd
tools\menuconfig.cmd host
```

```text
python tools/fwconfig.py sync --profile f407ve
python tools/fwconfig.py menuconfig --profile f407ve
python tools/fwconfig.py check --all
python tools/fwconfig.py check-layout
python tools/fwconfig.py check-manifest
python tools/fwconfig.py check-hygiene
python tools/fwconfig.py gen-keil --profile f407ve
python tools/fwconfig.py gen-keil --profile f407ve-hse
```

- `sync`：`.config` 不存在时从 `defconfig` 复制，再生成 `generated/`；
  同时删除旧的 boardgen 生成物（`board_config.h`、`board_desc.h`、
  `board_memory.ld`、`board_keil.json`、引脚/通道摘要）。
- `menuconfig`：编辑 profile 的 `.config`，退出后自动 sync。
- `check`：验证 generated 与 `.config` 一致；残留旧生成物视为 STALE。
- `check-layout`：拒绝旧的配置路径、`boards/`、`chips/`、`profile.json`、
  `tools/boardgen.py` 和拆分过细的旧板级模块。
- `check-manifest`：检查 `project/sources.json` 路径、重复和项目源文件覆盖。
- `check-hygiene`：拒绝第一方同名文件、示例式命名和旧 API 名字。
- `gen-keil`：从 source manifest + `board_config.h` + `board_policy.h` +
  模板生成 uvprojx；按 profile 输出 `keil/stm32f407ve*.uvprojx/.sct`；
  `--check` 只验证不写。
- `tools/tests/test_static_board.py`：静态校验 link.lds/Keil 内存/Stack/
  旧生成物/头文件纯度，由 CTest `v2_tools_tests` 执行。

## 生成文件

| 文件 | 用途 |
|---|---|
| `rtconfig.h` | RT-Thread 内核 |
| `board_policy.h` | 板级构建策略（Host profile 为空头） |
| `fw_config.h` | 框架组件与实例开关 |
| `app_config.h` | 应用层启动画面等配置 |
| `fw_config.cmake` | CMake 侧 `CONFIG_*` |
| `config_summary.txt` | 人工检查摘要 |

板级接线、内存区域、Keil Device/IROM/IRAM/Stack 都来自手写
`board_config.h`；`gen_keil.py` 直接解析它，不再有中间 JSON。

## 源文件清单

`project/sources.json` 是 CMake 与 Keil 共用的唯一源文件清单。新增/删除源文件只改
这一处；`check-manifest` 会检查项目目录下的 `.c` 是否都已登记。

## 命名约定

- Kconfig 符号：`RT_*`、`BOARD_*`、`FW_*`
- `.config` / CMake：`CONFIG_*`
- C 头文件：去掉 `CONFIG_` 前缀
- `RT_*` 的 false 布尔不定义（RT-Thread 用 `#ifdef`）
- `BOARD_*` / `FW_*` 的 false 布尔显式定义为 0（业务用 `#if`）
