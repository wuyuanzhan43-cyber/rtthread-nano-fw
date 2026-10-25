#!/usr/bin/env python3
"""管理单一配置源 profile 的命令行入口。

每个 profile 固定包含三样东西：

    profiles/<name>/defconfig    默认配置模板（人工维护）
    profiles/<name>/.config      当前配置（menuconfig 修改）
    profiles/<name>/generated/   生成的头文件和 CMake 片段

CMake 与 Keil 都消费同一个 profile；刻意不再有 per-build .config，
也不再有 keil/.config 和 keil/generated，避免两套配置漂移。

子命令：
    sync         确保 .config 存在，并生成 generated/
    menuconfig   打开 curses 菜单，退出后自动 sync
    check        只检查 generated/ 是否与 .config 一致
    check-layout 拒绝旧版配置路径与旧源码目录
    check-manifest 校验 project/sources.json
    check-hygiene 校验第一方同名文件与禁用命名
    gen-keil     从 manifest + 模板生成 uVision 工程

所有路径都相对于项目根目录解析，因此可以从任意工作目录调用。
"""

from __future__ import annotations

import argparse
import pathlib
import shutil
import subprocess
import sys

import genconfig as genconfig_mod

# 项目根目录 = tools/ 的上一级；后续所有路径都从这里派生。
ROOT = pathlib.Path(__file__).resolve().parent.parent
PROFILES = ROOT / "profiles"
KCONFIG = ROOT / "Kconfig"

# 被调用的辅助脚本；集中定义便于以后重命名/替换。
GENCONFIG = ROOT / "tools" / "genconfig.py"
MENUCONFIG = ROOT / "tools" / "menuconfig.py"
GEN_KEIL = ROOT / "tools" / "gen_keil.py"


def profile_dirs(name: str) -> tuple[pathlib.Path, pathlib.Path, pathlib.Path, pathlib.Path]:
    """返回 (profile 目录, defconfig, .config, generated 目录)。

    这里只计算路径，不检查存在性；调用者按需校验。
    """
    base = PROFILES / name
    return base, base / "defconfig", base / ".config", base / "generated"


def ensure_config(name: str) -> pathlib.Path:
    """确保 profile 存在且 .config 已生成。

    - profile 目录必须存在；
    - defconfig 必须存在（作为模板）；
    - .config 不存在时从 defconfig 复制一份。

    只复制一次：如果每次 sync 都覆盖 .config，menuconfig 的修改会丢失，
    而且 CMake 会因为文件变化无限重新配置。
    """
    base, defconfig, config, _ = profile_dirs(name)
    if not base.is_dir():
        raise SystemExit(f"profile '{name}' does not exist under {PROFILES}")
    if not defconfig.is_file():
        raise SystemExit(f"missing {defconfig}")
    if not config.is_file():
        shutil.copyfile(defconfig, config)
        print(f"profile {name}: seeded {config.relative_to(ROOT)}")
    return config


def run(args: list[str]) -> int:
    """在项目根目录运行子进程，返回退出码。"""
    return subprocess.run(args, cwd=ROOT).returncode


def sync(name: str) -> int:
    """从 .config 生成 profile 的头文件和 CMake 片段。

    genconfig.py 负责解析 Kconfig、校验配置并写出：
        rtconfig.h / board_policy.h / fw_config.h / fw_config.cmake
        config_summary.txt

    板级接线（引脚/通道/内存/LCD/NVM）在 hand-written board_config.h，
    不由生成器输出。
    """
    ensure_config(name)
    _, defconfig, config, generated = profile_dirs(name)
    return run([
        sys.executable, str(GENCONFIG),
        "--kconfig", str(KCONFIG),
        "--config", str(config),
        "--defconfig", str(defconfig),
        "--outdir", str(generated),
    ])


def menuconfig(name: str) -> int:
    """打开交互式菜单；正常退出后自动 sync。

    菜单进程返回非 0（例如 curses 不可用）时直接透传错误，
    不再生成配置，避免把半编辑状态写进 generated。
    """
    ensure_config(name)
    _, defconfig, config, _ = profile_dirs(name)
    rc = run([
        sys.executable, str(MENUCONFIG),
        "--kconfig", str(KCONFIG),
        "--config", str(config),
        "--defconfig", str(defconfig),
    ])
    if rc != 0:
        return rc
    return sync(name)


def check(name: str) -> int:
    """检查 generated/ 是否与 .config 一致（只读，不写文件）。

    genconfig 的 --check 会重新计算期望内容并与磁盘比较；
    不一致返回非 0，提示开发者运行 sync。
    """
    _, defconfig, config, generated = profile_dirs(name)
    if not config.is_file():
        print(f"profile {name}: missing {config.relative_to(ROOT)}", file=sys.stderr)
        return 1
    return run([
        sys.executable, str(GENCONFIG),
        "--kconfig", str(KCONFIG),
        "--config", str(config),
        "--defconfig", str(defconfig),
        "--outdir", str(generated),
        "--check",
    ])


def all_profiles() -> list[str]:
    """列出 profiles/ 下所有带 defconfig 的子目录。"""
    return sorted(p.name for p in PROFILES.iterdir() if (p / "defconfig").is_file())


def check_layout() -> int:
    """拒绝旧版配置布局。

    旧版曾在 configs/、keil/.config、keil/generated 或
    build/<preset>/.config 下保存配置，导致 CMake 与 Keil 不一致。
    这个检查让旧路径无法悄悄回归。
    """
    errors: list[str] = []
    forbidden = [
        ROOT / "configs",
        ROOT / "keil" / ".config",
        ROOT / "keil" / "generated",
        # 重构前的源码目录；任何一项重新出现都说明旧布局回归。
        ROOT / "src" / "bsp",
        ROOT / "src" / "board",
        ROOT / "src" / "framework" / "app",
        ROOT / "src" / "framework" / "ports",
        # 通用启动/console/IRQ 已下沉到 platform/stm32f407。
        ROOT / "src" / "platform" / "board" / "stm32f407ve" / "board_console.c",
        ROOT / "src" / "platform" / "board" / "stm32f407ve" / "board_irq.c",
        # manifest/boardgen 时代已删除；板级接线只在 board_config.h。
        ROOT / "boards",
        ROOT / "chips",
        ROOT / "tools" / "boardgen.py",
        ROOT / "tools" / "tests" / "test_boardgen.py",
        # 拆分过细的旧板级模块。
        ROOT / "src" / "platform" / "board" / "stm32f407ve" / "board_devices.c",
        ROOT / "src" / "platform" / "board" / "stm32f407ve" / "board_peripherals.c",
        ROOT / "src" / "platform" / "board" / "stm32f407ve" / "board_display.c",
        ROOT / "src" / "platform" / "board" / "stm32f407ve" / "board_storage.c",
        ROOT / "src" / "platform" / "board" / "stm32f407ve" / "board_pins.c",
        ROOT / "src" / "platform" / "board" / "stm32f407ve" / "board_pins.h",
        ROOT / "src" / "platform" / "board" / "stm32f407ve" / "board_clock.c",
        ROOT / "src" / "platform" / "board" / "stm32f407ve" / "board_clock.h",
        ROOT / "src" / "platform" / "board" / "stm32f407ve" / "board_init.h",
    ]

    # profile 只保留 defconfig/.config/generated 策略；旧 manifest 选择已删除。
    for profile in PROFILES.iterdir():
        if not profile.is_dir():
            continue
        forbidden.append(profile / "profile.json")
        for name in ("board_config.h", "board_desc.h", "board_memory.ld",
                     "board_keil.json", "board_summary.txt", "board_pinout.md",
                     "board_channels.md"):
            forbidden.append(profile / "generated" / name)
    for path in forbidden:
        if path.exists():
            errors.append(f"legacy path still exists: {path.relative_to(ROOT)}")

    # build/ 下也不允许出现配置副本；它们只应该存在于 profiles/。
    for build in (ROOT / "build").glob("*"):
        if not build.is_dir():
            continue
        for name in (".config", "generated"):
            path = build / name
            if path.exists():
                errors.append(f"per-build config still exists: {path.relative_to(ROOT)}")

    if errors:
        for error in errors:
            print(f"layout: {error}", file=sys.stderr)
        return 1
    print("layout: OK (profiles only)")
    return 0


def gen_keil(name: str, check: bool = False) -> int:
    """调用 gen_keil.py 生成或校验 uVision 工程。"""
    if not GEN_KEIL.is_file():
        print(f"missing {GEN_KEIL.relative_to(ROOT)}", file=sys.stderr)
        return 1
    args = [sys.executable, str(GEN_KEIL), "--profile", name]
    if check:
        args.append("--check")
    return run(args)


# manifest 中被视为“源文件列表”的键。
# includes/defines 不在这里检查覆盖性，但仍由 gen_keil 读取。
SOURCE_KEYS = [
    "core_sources", "protocol_sources", "display_sources", "task_sources",
    "app_sources",
    "assets_sources", "rtport_sources", "main_sources", "host_tests",
    "bsp_sources", "board_sources", "rtt_common_sources",
    "rtt_gcc_sources", "rtt_ac5_sources", "hal_sources",
    "gcc_startup", "keil_startup",
]


# 卫生检查：第一方源码中不允许再出现的旧名字或旧路径。
FORBIDDEN_TOKENS = [
    "demo",
    "fw_demo",
    "st7735s_demo",
    "board_boot_display",
    "mr_board",
    "framework/app",
    "src/bsp",
    "src/board",
]

# 只扫描文本文件，避免读入二进制对象/图片。
TEXT_SUFFIXES = {
    ".c", ".h", ".s", ".md", ".html", ".py", ".json", ".txt",
    ".cmake", ".uvprojx", ".uvoptx", ".sct", ".lds", ".ld", ".config",
    ".cmd", ".gitignore", ".editorconfig",
}
TEXT_NAMES = {"defconfig", "Kconfig", "CMakeLists.txt", "README.md",
              ".config", ".gitignore", ".editorconfig"}


def _is_text(path: pathlib.Path) -> bool:
    """判断文件是否需要做文本扫描。"""
    if path.name in TEXT_NAMES or path.name.startswith("Kconfig"):
        return True
    return path.suffix.lower() in TEXT_SUFFIXES


def _first_party_paths():
    """遍历仓库文本文件，排除 build、vendor、third_party、kconfiglib 和缓存。"""
    skipped = {".git", "build", "vendor", "third_party", "kconfiglib", "__pycache__",
               "Objects", "Listings"}
    for path in ROOT.rglob("*"):
        if not path.is_file():
            continue
        rel = path.relative_to(ROOT)
        if any(part in skipped for part in rel.parts):
            continue
        if _is_text(path):
            yield path, rel


def check_hygiene() -> int:
    """检查第一方同名文件与旧命名残留。

    检查项：
      1. src/ 下第一方 .c/.h/.s/Kconfig*/README.md 不允许同名；
      2. 全仓库文本文件不允许出现 demo、旧路径或旧 API 名字。

    vendor/、third_party/、tools/kconfiglib 属于外部代码，跳过。
    """
    errors: list[str] = []
    basenames: dict[str, list[str]] = {}

    for path in (ROOT / "src").rglob("*"):
        if not path.is_file():
            continue
        rel = path.relative_to(ROOT)
        if any(part in {"vendor", "third_party", "__pycache__"} for part in rel.parts):
            continue
        name = path.name
        if path.suffix.lower() in {".c", ".h", ".s"} or name.startswith("Kconfig") \
                or name == "README.md":
            basenames.setdefault(name, []).append(rel.as_posix())

    for name, paths in sorted(basenames.items()):
        if len(paths) > 1:
            errors.append(f"duplicate basename {name}: {', '.join(paths)}")

    for path, rel in _first_party_paths():
        # 本文件包含 FORBIDDEN_TOKENS 定义本身，不能扫描自己。
        if rel.as_posix() == "tools/fwconfig.py":
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except (UnicodeDecodeError, OSError):
            continue
        lowered = text.lower()
        for token in FORBIDDEN_TOKENS:
            if token.lower() in lowered:
                errors.append(f"forbidden token '{token}': {rel.as_posix()}")

    if errors:
        for error in errors:
            print(f"hygiene: {error}", file=sys.stderr)
        return 1
    print("hygiene: OK (unique basenames, no legacy names)")
    return 0


def check_manifest() -> int:
    """校验 project/sources.json。

    检查项：
      1. 所有列出的文件真实存在；
      2. 同一个文件不会同时出现在两个分组里；
      3. src/ 下的每个 .c 都在 manifest 中登记（防止新文件漏编）。

    这保证 CMake 和 Keil 的源文件列表来自同一份数据，
    且新增/删除文件时不会出现“能编但漏文件”或“文件不存在”的问题。
    """
    import json

    manifest_path = ROOT / "project" / "sources.json"
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except Exception as exc:
        print(f"manifest: {exc}", file=sys.stderr)
        return 1

    errors: list[str] = []
    seen: dict[str, str] = {}
    all_files: set[str] = set()

    for key in SOURCE_KEYS:
        for item in manifest.get(key, []):
            path = ROOT / item
            if not path.is_file():
                errors.append(f"{key}: missing {item}")
            if item in seen:
                errors.append(f"{item} appears in both {seen[item]} and {key}")
            seen[item] = key
            all_files.add(item)

    # 反向检查：项目自有目录中的 .c 必须全部登记。
    project_roots = ["src/app", "src/framework", "src/platform", "src/tests/host"]
    for root_name in project_roots:
        root = ROOT / root_name
        if not root.is_dir():
            continue
        for path in root.rglob("*.c"):
            rel = path.relative_to(ROOT).as_posix()
            if rel not in all_files:
                errors.append(f"project source not in manifest: {rel}")

    if errors:
        for error in errors:
            print(f"manifest: {error}", file=sys.stderr)
        return 1
    print(f"manifest: OK ({len(all_files)} entries)")
    return 0


def main(argv: list[str]) -> int:
    """解析子命令并分发。"""
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)

    # sync/check 支持单个 profile 或 --all。
    p_sync = sub.add_parser("sync", help="seed .config and generate profile headers")
    p_sync.add_argument("--profile")
    p_sync.add_argument("--all", action="store_true")

    p_menu = sub.add_parser("menuconfig", help="edit a profile and regenerate")
    p_menu.add_argument("--profile", required=True)

    p_check = sub.add_parser("check", help="verify generated headers are current")
    p_check.add_argument("--profile")
    p_check.add_argument("--all", action="store_true")

    sub.add_parser("check-layout", help="reject legacy config and source paths")
    sub.add_parser("check-manifest", help="verify sources.json paths and coverage")
    sub.add_parser("check-hygiene", help="reject duplicate basenames and legacy names")

    p_keil = sub.add_parser("gen-keil", help="generate the AC5 uVision project")
    p_keil.add_argument("--profile", required=True)
    p_keil.add_argument("--check", action="store_true")

    args = parser.parse_args(argv)

    if args.command == "sync":
        names = all_profiles() if args.all else ([args.profile] if args.profile else [])
        if not names:
            parser.error("sync needs --profile NAME or --all")
        # 所有 profile 都执行；返回最严重的退出码。
        return max((sync(n) for n in names), default=0)

    if args.command == "menuconfig":
        return menuconfig(args.profile)

    if args.command == "check":
        names = all_profiles() if args.all else ([args.profile] if args.profile else [])
        if not names:
            parser.error("check needs --profile NAME or --all")
        rc = 0
        for name in names:
            result = check(name)
            print(f"profile {name}: {'OK' if result == 0 else 'STALE'}")
            rc = max(rc, result)
        return rc

    if args.command == "check-layout":
        return check_layout()

    if args.command == "check-manifest":
        return check_manifest()

    if args.command == "check-hygiene":
        return check_hygiene()

    if args.command == "gen-keil":
        return gen_keil(args.profile, args.check)

    return 1


if __name__ == "__main__":
    # 用退出码向 CMake/CI 报告结果。
    raise SystemExit(main(sys.argv[1:]))
