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
import pathlib
import re
import sys

# 项目根目录和固定输入/输出路径。
ROOT = pathlib.Path(__file__).resolve().parent.parent
MANIFEST = ROOT / "project" / "sources.json"
TEMPLATE = ROOT / "keil" / "project_template.uvprojx"
DEFAULT_OUTPUT = ROOT / "keil" / "stm32f407ve.uvprojx"

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


def render(manifest: dict, profile: str) -> str:
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
        ("src/app", list_value(manifest, "main_sources")),
        ("src/board/stm32f407ve", list_value(manifest, "board_sources")),
        ("src/bsp/st", list_value(manifest, "bsp_sources")),
        ("src/framework/core", list_value(manifest, "core_sources")),
        ("src/framework/components",
         list_value(manifest, "protocol_sources") + list_value(manifest, "display_sources")),
        ("src/framework/app",
         list_value(manifest, "app_sources") + list_value(manifest, "assets_sources")),
        ("src/framework/ports/rtthread", list_value(manifest, "rtport_sources")),
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

    return text


def main(argv: list[str]) -> int:
    """生成或校验 uvprojx。

    --profile  选择 profiles/<name>/generated 作为第一个 include 路径；
    --check    不写文件，只比较现有工程与期望内容；
    --output   可选的输出路径（默认 keil/stm32f407ve.uvprojx）。
    """
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", default="f407ve")
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--output", default=str(DEFAULT_OUTPUT))
    args = parser.parse_args(argv)

    expected = render(load_manifest(), args.profile)
    output = pathlib.Path(args.output)

    if args.check:
        try:
            current = output.read_text(encoding="utf-8-sig")
        except FileNotFoundError:
            print(f"gen_keil: missing {output.relative_to(ROOT)}", file=sys.stderr)
            return 1
        if current != expected:
            print(f"gen_keil: {output.relative_to(ROOT)} is stale; run tools/gen_keil.py",
                  file=sys.stderr)
            return 1
        print("gen_keil: OK")
        return 0

    # uVision 工程使用 CRLF；显式指定 newline 保证跨平台生成一致。
    with output.open("w", encoding="utf-8", newline="\r\n") as handle:
        handle.write(expected)
    print(f"gen_keil: wrote {output.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
