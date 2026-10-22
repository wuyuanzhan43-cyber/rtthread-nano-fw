#!/usr/bin/env python3
"""从 project/sources.json 生成 Keil AC5 uVision 工程。

设计目标：
    - CMake 与 Keil 共用同一份源文件清单，避免两套列表漂移；
    - 模板 project_template.uvprojx 只保存固定选项（器件、AC5、C99、
      输出目录、scatter 文件），生成器只替换分组、包含路径和宏定义；
    - --check 只比较不写，供 CTest 验证生成的工程是否最新。

生成物：keil/stm32f407ve.uvprojx

注意：
    不要在 Keil GUI 里手工修改工程分组/包含路径——下次生成会被覆盖；
    要改就改 sources.json 或模板。
"""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys

# 项目根目录和固定输入/输出路径。
ROOT = pathlib.Path(__file__).resolve().parent.parent
MANIFEST = ROOT / "project" / "sources.json"
TEMPLATE = ROOT / "keil" / "project_template.uvprojx"
DEFAULT_OUTPUT = ROOT / "keil" / "stm32f407ve.uvprojx"
SCATTER_TEMPLATE = ROOT / "keil" / "scatter_template.sct"
DEFAULT_SCATTER = ROOT / "keil" / "stm32f407ve.sct"
STARTUP = ROOT / "keil" / "startup_stm32f407xx.s"

# uVision 工程里的路径分隔符。
BS = "\\"


def load_manifest() -> dict:
    """读取 JSON 清单；文件缺失/语法错误会直接抛异常，由调用者终止。"""
    import json
    with MANIFEST.open(encoding="utf-8") as handle:
        return json.load(handle)


def list_value(manifest: dict, key: str) -> list[str]:
    """读取清单中的字符串列表；类型不对时报错，避免生成半截工程。"""
    value = manifest.get(key, [])
    if not isinstance(value, list):
        raise SystemExit(f"manifest key {key} must be a list")
    return value


def keil_path(path: str) -> str:
    """把项目相对路径转换成“相对 uvprojx 所在目录”的 Windows 路径。

    uvprojx 位于 keil/ 下：
        keil/startup_stm32f407xx.s -> startup_stm32f407xx.s
        src/app/main.c             -> ..\\src\\app\\main.c

    使用 ..\\ 前缀而不是绝对路径，项目移动后工程仍然可用。
    """
    if path.startswith("keil/"):
        return path[len("keil/"):].replace("/", BS)
    return ".." + BS + path.replace("/", BS)


def group_xml(name: str, sources: list[str]) -> list[str]:
    """为一个分组生成 <Group>...</Group> XML 片段。

    每个源文件：
        FileName - 显示名（只取文件名，避免同名文件在 Keil 里混淆时
                   还能靠分组区分）；
        FileType - 2 表示汇编 .s，1 表示 C；
        FilePath - 相对工程文件的路径。

    生成前检查文件存在，尽早发现 manifest 漏项/路径错误。
    """
    lines = [
        "        <Group>",
        f"          <GroupName>{name}</GroupName>",
        "          <Files>",
    ]
    for source in sources:
        path = ROOT / source
        if not path.is_file():
            raise SystemExit(f"missing source: {source}")
        file_type = 2 if source.lower().endswith((".s",)) else 1
        lines += [
            "            <File>",
            f"              <FileName>{path.name}</FileName>",
            f"              <FileType>{file_type}</FileType>",
            f"              <FilePath>{keil_path(source)}</FilePath>",
            "            </File>",
        ]
    lines += ["          </Files>", "        </Group>"]
    return lines


def _keil_info(profile: str) -> dict:
    """读取 profiles/<profile>/generated/board_keil.json。"""
    path = ROOT / "profiles" / profile / "generated" / "board_keil.json"
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        raise SystemExit(f"gen_keil: missing {path.relative_to(ROOT)}")
    except json.JSONDecodeError as error:
        raise SystemExit(f"gen_keil: invalid {path.relative_to(ROOT)}: {error}")


def _display_path(path: pathlib.Path) -> str:
    try:
        return str(path.resolve().relative_to(ROOT))
    except ValueError:
        return str(path.resolve())


def render(manifest: dict, profile: str, keil_info: dict,
           scatter_name: str) -> str:
    """根据 manifest 和模板渲染完整 uvprojx 文本。

    分组映射与 CMake 目标大致对应，但 Keil 需要把“同一目录下的源文件”
    放在一个 Group 里，因此这里按目录/组件划分，而不是按 CMake 库划分。

    includes 顺序：
        1. profiles/<profile>/generated（必须第一，rtconfig.h 等优先）；
        2. manifest.includes（项目自有头文件目录）；
        3. manifest.target_includes（Nano、HAL、CMSIS）。
    """
    if not TEMPLATE.is_file():
        raise SystemExit(f"missing template: {TEMPLATE.relative_to(ROOT)}")
    text = TEMPLATE.read_text(encoding="utf-8-sig")

    # 分组名只影响 Keil 工程树显示；文件路径由 manifest 决定。
    groups = [
        ("src/app", list_value(manifest, "main_sources") + list_value(manifest, "app_sources")),
        ("src/app/assets", list_value(manifest, "assets_sources")),
        ("src/platform/board/stm32f407ve", list_value(manifest, "board_sources")),
        ("src/platform/stm32f407", list_value(manifest, "bsp_sources")),
        ("src/framework/core", list_value(manifest, "core_sources")),
        ("src/framework/components",
         list_value(manifest, "protocol_sources")
         + list_value(manifest, "display_sources")
         + list_value(manifest, "task_sources")),
        ("src/framework/port/rtthread", list_value(manifest, "rtport_sources")),
        ("src/vendor/rt-thread-nano",
         list_value(manifest, "rtt_common_sources") + list_value(manifest, "rtt_ac5_sources")),
        ("src/third_party/HAL", list_value(manifest, "hal_sources")),
        ("keil/startup", list_value(manifest, "keil_startup")),
    ]

    lines = ["      <Groups>"]
    for name, sources in groups:
        lines += group_xml(name, sources)
    lines.append("      </Groups>")
    groups_xml = "\n".join(lines)

    # generated 目录必须排在最前：rtconfig.h / board_config.h 会被
    # vendor/chip 头文件按名字包含，搜索顺序不能变。
    include_paths = ["profiles/" + profile + "/generated"]
    include_paths += list_value(manifest, "includes")
    include_paths += list_value(manifest, "target_includes")
    include_string = ";".join(keil_path(item) for item in include_paths)
    define_string = ";".join(list_value(manifest, "defines"))

    # 模板中的 <Groups> 整段替换；<Cads> 下的 IncludePath/Define 也替换。
    text, count = re.subn(r"<Groups>.*?</Groups>", lambda _: groups_xml, text, count=1, flags=re.S)
    if count != 1:
        raise SystemExit("template has no <Groups> section")

    text, count = re.subn(
        r"(<Cads>.*?<IncludePath>).*?(</IncludePath>)",
        lambda m: m.group(1) + include_string + m.group(2), text, count=1, flags=re.S)
    if count != 1:
        raise SystemExit("template has no C include path section")

    text, count = re.subn(
        r"(<Cads>.*?<Define>).*?(</Define>)",
        lambda m: m.group(1) + define_string + m.group(2), text, count=1, flags=re.S)
    if count != 1:
        raise SystemExit("template has no C define section")

    replacements = {
        "@DEVICE@": keil_info["device"],
        "@VENDOR@": keil_info["vendor"],
        "@CPU@": keil_info["cpu"],
        "@FLASH_DLL@": keil_info["flash_dll"],
        "@REGISTER_FILE@": keil_info["register_file"],
        "@SVD_FILE@": keil_info["svd_file"],
        "@FLASH_ORIGIN@": keil_info["flash_origin"],
        "@RAM_ORIGIN@": keil_info["ram_origin"],
        "@FLASH_SIZE@": keil_info["flash_size"],
        "@RAM_SIZE@": keil_info["ram_size"],
        "@FLASH_ORIGIN_BARE@": keil_info["flash_origin_bare"],
        "@FLASH_SIZE_BARE@": keil_info["flash_size_bare"],
        "@RAM_SIZE_BARE@": keil_info["ram_size_bare"],
        "@SCATTER_NAME@": scatter_name,
        "@BOARD_ID@": keil_info["board_id"],
    }
    for key, value in replacements.items():
        text = text.replace(key, str(value))
    leftover = re.findall(r"@[A-Z0-9_]+@", text)
    if leftover:
        raise SystemExit(f"template placeholders not replaced: {sorted(set(leftover))}")
    return text


def _board_memory_value(text: str, name: str) -> int:
    """从生成的 board_config.h 读取整型/十六进制 BOARD_* 值。"""
    match = re.search(rf"#define\s+{name}\s+(0x[0-9A-Fa-f]+|\d+)", text)
    if match is None:
        raise SystemExit(f"gen_keil: board_config.h is missing {name}")
    return int(match.group(1), 0)


def _board_config_text(profile: str) -> str:
    """读取 profiles/<profile>/generated/board_config.h。"""
    config = ROOT / "profiles" / profile / "generated" / "board_config.h"
    try:
        return config.read_text(encoding="utf-8")
    except FileNotFoundError:
        raise SystemExit(f"gen_keil: missing {config.relative_to(ROOT)}")


def render_scatter(keil_info: dict) -> str:
    """用 board_keil.json 的地址和大小渲染 Keil scatter。"""
    template = SCATTER_TEMPLATE.read_text(encoding="utf-8")
    return (template
            .replace("@FLASH_ORIGIN@", str(keil_info["flash_origin"]))
            .replace("@RAM_ORIGIN@", str(keil_info["ram_origin"]))
            .replace("@FLASH_SIZE@", str(keil_info["flash_size"]))
            .replace("@RAM_SIZE@", str(keil_info["ram_size"])))


def render_startup(profile: str) -> bytes:
    """把 board_config.h 的 BOARD_MIN_STACK_SIZE 同步到 Keil 启动文件。

    只替换 Stack_Size 一行，并保留原文件的 CRLF/LF，避免整文件行尾变化。
    """
    text = _board_config_text(profile)
    stack_size = _board_memory_value(text, "BOARD_MIN_STACK_SIZE")
    try:
        raw = STARTUP.read_bytes()
    except FileNotFoundError:
        raise SystemExit(f"gen_keil: missing {STARTUP.relative_to(ROOT)}")

    pattern = rb"(?m)^(\s*Stack_Size\s+EQU\s+)0x[0-9A-Fa-f]+"
    rendered, count = re.subn(
        pattern,
        lambda match: match.group(1) + f"0x{stack_size:08X}".encode("ascii"),
        raw,
        count=1)
    if count != 1:
        raise SystemExit("gen_keil: startup has no Stack_Size EQU line")
    return rendered


def main(argv: list[str]) -> int:
    """生成或校验 uvprojx。

    --profile  选择 profiles/<name>/generated 作为第一个 include 路径；
    --check    不写文件，只比较现有工程与期望内容；
    --output   可选的输出路径（默认 keil/stm32f407ve.uvprojx）。
    """
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", default="f407ve")
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--output", default=None,
                        help="uvprojx 输出路径；默认按 profile 派生")
    args = parser.parse_args(argv)

    if args.profile == "f407ve":
        suffix = ""
    elif args.profile.startswith("f407ve-"):
        suffix = "-" + args.profile[len("f407ve-"):]
    else:
        suffix = "-" + args.profile
    default_output = ROOT / "keil" / f"stm32f407ve{suffix}.uvprojx"
    scatter_output = ROOT / "keil" / f"stm32f407ve{suffix}.sct"
    output = pathlib.Path(args.output) if args.output else default_output

    keil_info = _keil_info(args.profile)
    expected = render(load_manifest(), args.profile, keil_info,
                      scatter_output.name)
    scatter = render_scatter(keil_info)
    startup = render_startup(args.profile)

    if args.check:
        try:
            current = output.read_text(encoding="utf-8-sig")
            current_scatter = scatter_output.read_text(encoding="utf-8-sig")
            current_startup = STARTUP.read_bytes()
        except FileNotFoundError as error:
            print(f"gen_keil: missing {error.filename}", file=sys.stderr)
            return 1
        if current != expected:
            print(f"gen_keil: {_display_path(output)} is stale; run tools/gen_keil.py",
                  file=sys.stderr)
            return 1
        if current_scatter != scatter:
            print(f"gen_keil: {_display_path(scatter_output)} is stale; run tools/gen_keil.py",
                  file=sys.stderr)
            return 1
        if current_startup != startup:
            print(f"gen_keil: {_display_path(STARTUP)} is stale; run tools/gen_keil.py",
                  file=sys.stderr)
            return 1
        print("gen_keil: OK")
        return 0

    # uVision 工程使用 CRLF；显式指定 newline 保证跨平台生成一致。
    with output.open("w", encoding="utf-8", newline="\r\n") as handle:
        handle.write(expected)
    with scatter_output.open("w", encoding="utf-8", newline="\r\n") as handle:
        handle.write(scatter)
    STARTUP.write_bytes(startup)
    print(f"gen_keil: wrote {_display_path(output)}, "
          f"{_display_path(scatter_output)}, {_display_path(STARTUP)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
