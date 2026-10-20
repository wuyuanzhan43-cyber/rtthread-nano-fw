# Keil MDK（ARMCC 5 / AC5）

工程文件：

```text
keil/stm32f407ve.uvprojx
```

工程由 `project/sources.json` 和 `keil/project_template.uvprojx` 生成；配置与
CMake 目标构建共用：

```text
profiles/f407ve/.config
profiles/f407ve/generated/
```

不再有 `keil/.config` 或 `keil/generated/`。

## 已验证环境

| 项目 | 值 |
|---|---|
| Keil | UV4 |
| Compiler | ARMCC V5.06 update 5 (build 528) |
| Device | STM32F407VETx |
| Pack | `Keil.STM32F4xx_DFP.3.1.1` |
| 结果 | 0 Error(s), 0 Warning(s) |
| 产物 | `build/keil/fw_nano.axf`、`fw_nano.hex`、`fw_nano.map` |
| 尺寸 | Code 27,844 B · RO 42,076 B · RW 164 B · ZI 11,276 B |

## 1. 打开工程

1. 安装 Keil MDK 5 和 ARM Compiler 5（AC5）。
2. 在 Pack Installer 中确认已安装 `Keil.STM32F4xx_DFP`（工程使用 3.1.1）。
3. 打开 `keil/stm32f407ve.uvprojx`，Target 选择 `Debug`。

> 工程按 AC5 配置（`uAC6=0`）。AC6/ARMCLANG 需要单独的启动文件、
> `context_rvds.S` 和 toolset 适配。

## 2. 生成/更新配置与工程

修改 Kconfig 后：

```text
python tools/fwconfig.py sync --profile f407ve
python tools/fwconfig.py gen-keil --profile f407ve
```

- `sync` 只更新 `profiles/f407ve/.config` 对应的 generated 头文件；
- `gen-keil` 从 manifest + 模板重新生成 uvprojx。

交互式修改 Keil 使用的配置：

```text
python tools/fwconfig.py menuconfig --profile f407ve
```

注意：`cmake --build build/f407-debug --target menuconfig` 修改的是同一个
`profiles/f407ve/.config`，不是另一份配置。

## 3. 编译

GUI：按 `F7`。

命令行：

```text
UV4.exe -b keil\stm32f407ve.uvprojx -t Debug -j0 -o build\keil\uv4-build.log
```

产物在 `build/keil/`。如果需要 `.bin`：

```text
fromelf --bin --output build\keil\fw_nano.bin build\keil\fw_nano.axf
```

## 4. 烧录

1. `Project → Options for Target → Debug`，选择实际下载器：
   ST-Link / CMSIS-DAP / J-Link。
2. `Utilities` 勾选 `Use Debug Driver`。
3. `Settings → Flash Download` 确认算法为 `STM32F4xx 512KB Flash`，
   勾选 `Reset and Run`。
4. 按 `F8` 下载，`Ctrl+F5` 调试。

也可以直接用 STM32CubeProgrammer / ST-Link Utility 打开
`build/keil/fw_nano.hex`。

命令行烧录（需先在 GUI 配置下载器）：

```text
UV4.exe -f keil\stm32f407ve.uvprojx -j0
```

## 5. 文件说明

| 文件 | 作用 |
|---|---|
| `project_template.uvprojx` | 固定 AC5/器件/scatter/输出选项的模板 |
| `stm32f407ve.uvprojx` | 由 `tools/gen_keil.py` 生成的工程 |
| `startup_stm32f407xx.s` | ARM 语法启动文件，Stack_Size = 0x1000 |
| `stm32f407ve.sct` | 512 KB Flash / 128 KB SRAM scatter |

`profiles/f407ve/generated/` 是 include 路径中的配置头文件。

启动链路：

```text
startup __main
  → RT-Thread Nano $Sub$$main
  → rtthread_startup()
  → main_thread_entry()
  → 用户 main()（$Super$$main）
```

## 6. 常见问题

| 现象 | 处理 |
|---|---|
| 提示缺少 device pack | Pack Installer 安装 `Keil.STM32F4xx_DFP` |
| 汇编报 A1023E | 确认 `src/vendor/rt-thread-nano/libcpu/arm/cortex-m4/context_rvds.S` 存在 |
| 配置修改后不生效 | 运行 `fwconfig.py sync --profile f407ve`，再 Rebuild |
| 手工改 uvprojx 后检查失败 | 改 `project/sources.json` 或模板，再 `gen-keil` |
| 只有 AC6 | 当前工程不适用；需要 ARMCLANG 适配 |
