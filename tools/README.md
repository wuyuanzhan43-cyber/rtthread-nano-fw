# 配置与工程生成工具

`kconfiglib.py` 与 `menuconfig.py` 已 vendor 到 `tools/kconfiglib/`；Windows
的交互式 menuconfig 需要 curses：

```text
python -m pip install -r tools/requirements.txt
```

## 唯一配置源：profiles

```text
profiles/host/{defconfig,.config,generated/}
profiles/f407ve/{defconfig,.config,generated/}
profiles/f407ve-hse/{defconfig,.config,generated/}
```

CMake 目标构建和 Keil AC5 都消费 `profiles/f407ve/generated/`；不再有
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
python tools/fwconfig.py check-layout
python tools/fwconfig.py check-manifest
python tools/fwconfig.py gen-keil --profile f407ve
```

- `sync`：`.config` 不存在时从 `defconfig` 复制，再生成 `generated/`。
- `menuconfig`：编辑 profile 的 `.config`，退出后自动 sync。
- `check`：验证 generated 与 `.config` 一致。
- `check-layout`：拒绝旧的 per-build / keil 配置路径。
- `check-manifest`：检查 `project/sources.json` 路径、重复和项目源文件覆盖。
- `gen-keil`：从 manifest + `keil/project_template.uvprojx` 生成 uvprojx；
  `--check` 只验证不写。

## 生成文件

| 文件 | 用途 |
|---|---|
| `rtconfig.h` | RT-Thread 内核 |
| `board_config.h` | 板卡引脚/时钟 |
| `fw_config.h` | 框架组件与实例开关 |
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
