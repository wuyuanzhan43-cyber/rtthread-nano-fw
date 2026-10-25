#!/usr/bin/env python3
"""从 Kconfig 的 .config 生成 C 头文件、CMake 片段和配置摘要。

Kconfig 树是构建策略的唯一来源；板级接线在 board_config.h 手写。本脚本：
    1. 用 kconfiglib 解析 Kconfig 并载入 profile 的 .config；
    2. 把符号值分成 RT_ / BOARD_ / FW_ / APP_ 四组，生成对应头文件；
    3. 生成 CMake 侧 CONFIG_* 变量；
    4. 校验应用/组件组合、IRQ 优先级、时钟源、内存容量等策略约束；
    5. 支持 --check：只比较不写，供 CTest 防止生成物漂移；
    6. sync 时删除 boardgen 时代遗留的生成物。

调用者：
    - CMake 在 configure 阶段通过 tools/fwconfig.py sync 调用；
    - menuconfig 退出后由 fwconfig.py 自动调用；
    - 开发者可以手动调用。

重要约定：
    - 已存在的 .config 永远不会被本脚本覆盖（否则 CMake 会无限重配）；
    - 只有 .config 不存在时才从 defconfig 复制；
    - 写出前先比较内容，未变化不更新时间戳，避免触发无意义的重编译。
"""

from __future__ import annotations

import argparse
import os
import pathlib
import shutil
import sys
import tempfile

# tools/ 目录与 vendor 的 kconfiglib；不依赖 pip 安装。
HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE / "kconfiglib"))

import kconfiglib  # noqa: E402

# 手写 board_config.h 后，这些旧生成物必须从 generated 目录清除。
# generated 在 include 路径最前；残留会遮蔽手写头或造成内存/Keil 漂移。
OBSOLETE_OUTPUTS = (
    "board_config.h",
    "board_desc.h",
    "board_memory.ld",
    "board_keil.json",
    "board_summary.txt",
    "board_pinout.md",
    "board_channels.md",
)


def unknown_config_keys(config_path: pathlib.Path, kconf) -> list[str]:
    """返回 .config 中已不存在于 Kconfig 树的键。

    板级接线迁移到 board_config.h 后，旧的 CONFIG_BOARD_* 拓扑键
    必须被清理；否则它们会静默留在 .config 里，形成第二份事实。
    """
    known = {sym.name for sym in kconf.unique_defined_syms}
    unknown: list[str] = []
    for raw in config_path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line.startswith("CONFIG_") or "=" not in line:
            continue
        name = line[len("CONFIG_"):].split("=", 1)[0].strip()
        if name not in known:
            unknown.append(name)
    return unknown


def parse_args(argv: list[str]) -> argparse.Namespace:
    """定义命令行参数。

    --kconfig   顶层 Kconfig 文件；
    --config    当前 .config；
    --defconfig 模板（.config 缺失时复制）；
    --outdir    生成目录（profiles/<name>/generated）；
    --check     只验证生成物是否最新，不写文件。
    """
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kconfig", required=True, help="top-level Kconfig file")
    parser.add_argument("--config", required=True, help=".config path")
    parser.add_argument("--defconfig", required=True, help="fallback defconfig")
    parser.add_argument("--outdir", required=True, help="generated output directory")
    parser.add_argument("--check", action="store_true",
                        help="verify generated files are up to date; do not write")
    return parser.parse_args(argv)


def load_kconfig(kconfig_path: pathlib.Path) -> kconfiglib.Kconfig:
    """解析 Kconfig 树。

    kconfiglib 用环境变量 srctree 解析 source "..." 的相对路径；
    这里把它设为顶层 Kconfig 所在目录（项目根），
    因此 source "src/platform/stm32f407/Kconfig.drivers" 能正确找到文件。
    warn_to_stderr=False 避免默认警告污染 CMake 输出；
    真正的配置错误由 validate() 报告。
    """
    os.environ["srctree"] = str(kconfig_path.parent.resolve())
    return kconfiglib.Kconfig(str(kconfig_path), warn_to_stderr=False)


def symbol_value(sym: kconfiglib.Symbol):
    """把 kconfiglib 符号转换成 (kind, value) 二元组。

    kind:
        "bool"   - value 为 True/False；
        "int"    - value 为字符串形式的整数（写出时不再加引号）；
        "hex"    - 同上；
        "string" - value 为原始字符串。

    未设置的符号返回 None，由 collect_symbols 过滤。
    tristate 在本项目中等价于 bool（Nano 不支持模块）。
    """
    value = sym.str_value
    if sym.type == kconfiglib.BOOL:
        return ("bool", value == "y")
    if sym.type == kconfiglib.TRISTATE:
        return ("bool", value == "y")
    if sym.type == kconfiglib.INT:
        return ("int", value)
    if sym.type == kconfiglib.HEX:
        return ("hex", value)
    if sym.type == kconfiglib.STRING:
        return ("string", value)
    return None


def collect_symbols(kconf: kconfiglib.Kconfig) -> dict[str, tuple[str, object]]:
    """收集所有已定义符号的当前值。

    只遍历 unique_defined_syms，避免同一符号因多次 source 重复；
    过滤掉 value 为 None 或空字符串的非 bool 符号（未激活的 int/hex/string），
    避免生成 `#define NAME ` 或空 CMake 值。
    """
    result: dict[str, tuple[str, object]] = {}
    for sym in kconf.unique_defined_syms:
        if not sym.name or sym.name == "FW_PROJECT_VERSION":
            if sym.name:
                result[sym.name] = symbol_value(sym)
            continue
        parsed = symbol_value(sym)
        if parsed is not None:
            result[sym.name] = parsed

    def keep(item: tuple[str, object]) -> bool:
        kind, value = item
        if kind == "bool":
            return True
        return str(value) != ""

    return {k: v for k, v in result.items() if v is not None and keep(v)}


def header_guard(name: str) -> str:
    """由文件名生成头文件保护宏，例如 rtconfig.h -> FW_GENERATED_RTCONFIG_H。"""
    return "FW_GENERATED_" + name.upper().replace(".", "_").replace("-", "_")


def render_header(filename: str, title: str, symbols: dict[str, tuple[str, object]],
                  prefix: str) -> str:
    """生成一个 C 头文件。

    规则：
        - 只输出指定前缀的符号（RT_/BOARD_/FW_/APP_）；
        - bool 为真时写 #define NAME 1；
        - bool 为假时：
            RT_* 不定义（RT-Thread 用 #ifdef 判断特性）；
            BOARD_*/FW_*/APP_* 显式写 0（业务代码用 #if 判断）；
        - string 做 C 字符串转义；
        - int/hex 直接写数值。
    """
    lines = [
        "/*",
        f" * {filename} - generated by tools/genconfig.py. Do not edit.",
        f" * {title}",
        " */",
        f"#ifndef {header_guard(filename)}",
        f"#define {header_guard(filename)}",
        "",
    ]
    for name in sorted(symbols):
        if not name.startswith(prefix):
            continue
        kind, value = symbols[name]
        if kind == "bool":
            # RT-Thread 用 #ifdef 判断特性，因此 false 的 RT_* 必须保持未定义；
            # BOARD_*/FW_* 用 #if，显式 0 更容易阅读和调试。
            if value:
                lines.append(f"#define {name} 1")
            elif not name.startswith("RT_"):
                lines.append(f"#define {name} 0")
        elif kind == "string":
            escaped = str(value).replace("\\", "\\\\").replace('"', '\\"')
            lines.append(f'#define {name} "{escaped}"')
        else:
            lines.append(f"#define {name} {value}")
    lines += ["", f"#endif /* {header_guard(filename)} */", ""]
    return "\n".join(lines)


def render_cmake(symbols: dict[str, tuple[str, object]]) -> str:
    """生成 CMake include 片段。

    每个 Kconfig 符号变成 CONFIG_<NAME>：
        bool   -> ON/OFF
        string -> 带引号
        raw    -> 跳过（C 初始化列表，不参与 CMake）
        其他   -> 原值
    CMakeLists 通过 CONFIG_FW_PLATFORM_* 选择构建分支。
    """
    lines = [
        "# Generated by tools/genconfig.py. Do not edit.",
        "# CMake-side view of the Kconfig .config.",
        "",
    ]
    for name in sorted(symbols):
        kind, value = symbols[name]
        if kind == "raw":
            continue
        if kind == "bool":
            lines.append(f"set(CONFIG_{name} {'ON' if value else 'OFF'})")
        elif kind == "string":
            escaped = str(value).replace("\\", "\\\\").replace('"', '\\"')
            lines.append(f'set(CONFIG_{name} "{escaped}")')
        else:
            lines.append(f"set(CONFIG_{name} {value})")
    lines.append("")
    return "\n".join(lines)


def render_summary(target: str, symbols: dict[str, tuple[str, object]]) -> str:
    """生成人类可读的配置摘要，方便快速 diff/审查。"""
    lines = [f"# v2 configuration summary ({target})", ""]
    for name in sorted(symbols):
        kind, value = symbols[name]
        if kind == "bool":
            value = "y" if value else "n"
        lines.append(f"{name}={value}")
    lines.append("")
    return "\n".join(lines)


def validate(symbols: dict[str, tuple[str, object]]) -> list[str]:
    """校验配置组合，返回错误字符串列表。

    APP_* 在所有 profile 上校验；任务/RT 相关项只在 STM32 目标上校验。
    板级引脚/AF/内存由手写 board_config.h 和静态测试负责；这里只校验
    Kconfig 策略（时钟源、容量、时序、IRQ 优先级）：
    这里只保留：
      0. APP_BOOT_MODE 必须在 0..2；
      1. 必备组件开关必须为 y；
      2. RPC 等待、心跳 warn/lost 必须满足时序关系；
      3. 任务优先级必须小于 RT_THREAD_PRIORITY_MAX；
      4. 任务表和设备表容量必须覆盖 app 的最小需求。
    """
    errors: list[str] = []

    # 应用层配置与平台无关，Host profile 也要校验。
    boot_mode = symbols.get("APP_BOOT_MODE")
    if boot_mode is not None:
        try:
            mode_value = int(boot_mode[1])
        except (TypeError, ValueError):
            errors.append(f"APP_BOOT_MODE is not an integer: {boot_mode[1]}")
        else:
            if mode_value not in (0, 1, 2):
                errors.append(f"APP_BOOT_MODE must be 0..2, got {mode_value}")

    target = bool(symbols.get("FW_PLATFORM_STM32F407VE", ("bool", False))[1])
    if not target:
        return errors

    def val(name: str) -> int:
        """读取整数符号；缺失/非法时记录错误并返回 0。"""
        item = symbols.get(name)
        if item is None:
            errors.append(f"missing symbol {name}")
            return 0
        try:
            return int(item[1])
        except (TypeError, ValueError):
            errors.append(f"{name} is not an integer: {item[1]}")
            return 0

    def enabled(name: str) -> bool:
        """布尔符号是否为 y；缺失视为 False。"""
        return bool(symbols.get(name, ("bool", False))[1])

    # v2 第一版固定包含协议、显示和任务管理组件。
    for name in ("FW_USE_PROTOCOL", "FW_USE_DISPLAY", "FW_USE_TASK"):
        if not enabled(name):
            errors.append(f"{name} must be y in v2 first version")

    # SPI DMA 的完成同步依赖 RT-Thread 信号量（仅在驱动内部使用，
    # framework/core 仍然保持 OS 中立）。
    if any(enabled(f"FW_USING_SPI{number}_DMA") for number in (1, 2, 3)):
        if not enabled("RT_USING_SEMAPHORE"):
            errors.append("SPI DMA requires RT_USING_SEMAPHORE=y")

    # 任务/心跳/优先级交叉约束。单看每个范围都合法，组合起来却可能
    # 让 RPC 长等待越过心跳阈值、或让 fw_task_create 直接失败。
    task_max = val("FW_TASK_MAX")
    if task_max < 3:
        errors.append(f"FW_TASK_MAX must be >= 3 (main + rpc + worker), got {task_max}")

    # 设备表必须容纳所有启用的类实例：pin 一个、LCD 一个，
    # UART/SPI/I2C 每启用一个实例占一个槽位，ADC/PWM 每个控制器一个。
    need_devices = 1
    if enabled("BOARD_LCD_ENABLE"):
        need_devices += 1
    for number in (1, 2, 3):
        if enabled(f"FW_USING_UART{number}"):
            need_devices += 1
        if enabled(f"FW_USING_SPI{number}"):
            need_devices += 1
        if enabled(f"FW_USING_I2C{number}"):
            need_devices += 1
    if enabled("FW_USING_ADC1"):
        need_devices += 1
    if enabled("FW_USING_PWM4"):
        need_devices += 1
    dev_max = val("FW_DEV_MAX")
    if dev_max < need_devices:
        errors.append(
            f"FW_DEV_MAX must be >= {need_devices} for the enabled devices, "
            f"got {dev_max}")

    if enabled("FW_USING_ADC1"):
        bits = val("BOARD_ADC1_RESOLUTION_BITS")
        if bits not in (8, 10, 12):
            errors.append(f"BOARD_ADC1_RESOLUTION_BITS must be 8/10/12, got {bits}")
        cycles = val("BOARD_ADC1_SAMPLE_CYCLES")
        if cycles not in (3, 15, 28, 56, 84, 112, 144, 480):
            errors.append(
                "BOARD_ADC1_SAMPLE_CYCLES must be one of "
                f"3/15/28/56/84/112/144/480, got {cycles}")

    for number in (1, 2, 3):
        if enabled(f"FW_USING_I2C{number}"):
            speed = val(f"BOARD_I2C{number}_SPEED_HZ")
            if not 10000 <= speed <= 400000:
                errors.append(
                    f"BOARD_I2C{number}_SPEED_HZ must be 10000..400000, got {speed}")
    if enabled("FW_USING_PWM4"):
        pwm_hz = val("BOARD_PWM4_PERIOD_HZ")
        if pwm_hz <= 0:
            errors.append(f"BOARD_PWM4_PERIOD_HZ must be > 0, got {pwm_hz}")

    if enabled("BOARD_IWDG_ENABLE"):
        iwdg = val("BOARD_IWDG_TIMEOUT_MS")
        lost_ms = val("APP_TASK_HEARTBEAT_LOST_MS")
        refresh_ms = val("APP_HEALTH_REFRESH_MS")
        if iwdg <= lost_ms + refresh_ms:
            errors.append(
                "BOARD_IWDG_TIMEOUT_MS must be > "
                "APP_TASK_HEARTBEAT_LOST_MS + APP_HEALTH_REFRESH_MS; "
                f"got {iwdg} <= {lost_ms}+{refresh_ms}")

    hsi = enabled("BOARD_CLOCK_HSI_16")
    hse = enabled("BOARD_CLOCK_HSE_8_168")
    if hsi == hse:
        errors.append("exactly one of BOARD_CLOCK_HSI_16 / "
                      "BOARD_CLOCK_HSE_8_168 must be enabled")

    rpc_wait = val("APP_RPC_LOOP_WAIT_MAX_MS")
    worker_period = val("APP_WORKER_PERIOD_MS")
    hb_warn = val("APP_TASK_HEARTBEAT_WARN_MS")
    hb_lost = val("APP_TASK_HEARTBEAT_LOST_MS")
    if not 0 < rpc_wait < hb_warn < hb_lost:
        errors.append(
            "must satisfy APP_RPC_LOOP_WAIT_MAX_MS < "
            "APP_TASK_HEARTBEAT_WARN_MS < APP_TASK_HEARTBEAT_LOST_MS; "
            f"got {rpc_wait}/{hb_warn}/{hb_lost}")
    if not 0 < worker_period < hb_warn:
        errors.append(
            "APP_WORKER_PERIOD_MS must be < APP_TASK_HEARTBEAT_WARN_MS; "
            f"got {worker_period}/{hb_warn}")

    priority_max = val("RT_THREAD_PRIORITY_MAX")
    for name in ("RT_MAIN_THREAD_PRIORITY", "APP_RPC_TASK_PRIORITY",
                 "APP_WORKER_TASK_PRIORITY"):
        priority = val(name)
        if priority >= priority_max:
            errors.append(
                f"{name}={priority} must be < RT_THREAD_PRIORITY_MAX={priority_max}")

    worker_stack = val("APP_WORKER_TASK_STACK_SIZE")
    if worker_stack % 8 != 0:
        errors.append(
            f"APP_WORKER_TASK_STACK_SIZE must be a multiple of 8, got {worker_stack}")

    return errors


def write_if_changed(path: pathlib.Path, content: str) -> bool:
    """仅在内容变化时写文件，返回是否写入。

    使用 newline="\\n" 固定 LF，保证 Windows/Linux 生成的配置一致，
    也避免 CRLF 差异让 --check 误报。
    """
    try:
        if path.read_text(encoding="utf-8") == content:
            return False
    except FileNotFoundError:
        pass
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(content)
    return True


def render_board_policy(symbols: dict[str, tuple[str, object]]) -> str:
    """生成板级策略头。

    Host 没有板级策略：输出空头，让手写 board_config.h 的 #ifndef 默认值
    生效，避免 host 的 Kconfig 把 BOARD_CLOCK_* 等符号静默定义成 0。
    """
    if symbols.get("FW_PLATFORM_HOST", ("bool", False))[1]:
        return (
            "/*\n"
            " * board_policy.h - generated by tools/genconfig.py. Do not edit.\n"
            " * Host build: no board policy; board_config.h defaults apply.\n"
            " */\n"
            "#ifndef FW_GENERATED_BOARD_POLICY_H\n"
            "#define FW_GENERATED_BOARD_POLICY_H\n\n"
            "#endif /* FW_GENERATED_BOARD_POLICY_H */\n"
        )
    return render_header("board_policy.h", "Board build policy", symbols, "BOARD_")


def generate(symbols: dict[str, tuple[str, object]], outdir: pathlib.Path) -> dict[str, str]:
    """生成所有输出文件的内容（尚未写盘）。

    返回 {文件名: 内容}，便于 --check 与实际文件逐一比较。
    target 只用于摘要标题：host 或 stm32f407ve。
    """
    target = "host" if symbols.get("FW_PLATFORM_HOST", ("bool", False))[1] else "stm32f407ve"
    return {
        "rtconfig.h": render_header("rtconfig.h", "RT-Thread kernel configuration",
                                    symbols, "RT_"),
        # 板级接线在 board_config.h 手写；这里只输出 Kconfig 策略。
        "board_policy.h": render_board_policy(symbols),
        "fw_config.h": render_header("fw_config.h", "Framework component configuration",
                                     symbols, "FW_"),
        "app_config.h": render_header("app_config.h", "Application configuration",
                                      symbols, "APP_"),
        "fw_config.cmake": render_cmake(symbols),
        "config_summary.txt": render_summary(target, symbols),
    }


def main(argv: list[str]) -> int:
    """解析参数、载入配置、校验、生成或检查。"""
    args = parse_args(argv)
    kconfig_path = pathlib.Path(args.kconfig).resolve()
    config_path = pathlib.Path(args.config).resolve()
    defconfig_path = pathlib.Path(args.defconfig).resolve()
    outdir = pathlib.Path(args.outdir).resolve()

    # .config 不存在时从 defconfig 初始化；--check 模式不允许隐式创建。
    if not config_path.exists():
        if args.check:
            print(f"genconfig: missing {config_path}", file=sys.stderr)
            return 1
        config_path.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(defconfig_path, config_path)

    # 解析 Kconfig 并载入具体值；validate 在写文件之前执行，
    # 非法配置不会留下半生成的输出。
    kconf = load_kconfig(kconfig_path)
    kconf.load_config(str(config_path))
    symbols = collect_symbols(kconf)

    errors = validate(symbols)
    for name in unknown_config_keys(config_path, kconf):
        errors.append(f"unknown/removed .config key CONFIG_{name}; clean this profile")

    if errors:
        for error in errors:
            print(f"genconfig: {error}", file=sys.stderr)
        return 1

    outputs = generate(symbols, outdir)

    # --check：逐个比较内容；旧生成物残留也视为 STALE。
    if args.check:
        stale = []
        for name, content in outputs.items():
            path = outdir / name
            try:
                current = path.read_text(encoding="utf-8")
            except FileNotFoundError:
                current = None
            if current != content:
                stale.append(name)
        for name in OBSOLETE_OUTPUTS:
            if (outdir / name).exists():
                stale.append(name + " (obsolete)")
        if stale:
            print("genconfig: generated files are stale: " + ", ".join(stale),
                  file=sys.stderr)
            return 1
        return 0

    # 正常写入：先清理旧生成物，再只写变化的文件。
    outdir.mkdir(parents=True, exist_ok=True)
    removed = []
    for name in OBSOLETE_OUTPUTS:
        path = outdir / name
        if path.exists():
            path.unlink()
            removed.append(name)
    if removed:
        print("genconfig: removed obsolete " + ", ".join(removed))
    changed = []
    for name, content in outputs.items():
        if write_if_changed(outdir / name, content):
            changed.append(name)
    if changed:
        print("genconfig: wrote " + ", ".join(changed))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
