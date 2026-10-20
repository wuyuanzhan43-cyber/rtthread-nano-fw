#!/usr/bin/env python3
"""打开 Kconfig 的 curses 菜单，并把结果保存到 profile 的 .config。

Windows 需要安装 windows-curses：
    python -m pip install -r tools/requirements.txt

kconfiglib 本身已经 vendor 在 tools/kconfiglib/，不需要 pip 安装。

流程：
    1. 解析 Kconfig 树（srctree 设为项目根）；
    2. 如果 .config 存在就载入，否则从 defconfig 复制并立即写一份；
    3. 调用 kconfiglib 的 menuconfig 交互界面；
    4. 退出后写回 .config；
    5. fwconfig.py 会在本脚本返回后自动执行 sync，生成头文件。

按键（与 kconfiglib 菜单一致）：
    ↑/↓ 或 j/k  移动；→/Enter 进入；←/Esc 返回；
    Space 切换/修改；Y/N 直接设置；/ 搜索；? 帮助；
    q 或 Esc Esc 退出并保存。
"""

from __future__ import annotations

import argparse
import os
import pathlib
import sys

# 把 vendor 的 kconfiglib 加入搜索路径，避免依赖全局安装。
HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE / "kconfiglib"))

import kconfiglib  # noqa: E402


def main() -> int:
    """解析参数并运行菜单。返回 0 表示保存成功。"""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kconfig", required=True)
    parser.add_argument("--config", required=True)
    parser.add_argument("--defconfig", required=True)
    args = parser.parse_args()

    try:
        import menuconfig  # noqa: E402
    except ImportError:
        # 典型场景：Windows 上没装 windows-curses，_curses 模块不存在。
        print("menuconfig: curses UI is unavailable. On Windows run:\n"
              "  python -m pip install -r tools/requirements.txt",
              file=sys.stderr)
        return 1

    kconfig_path = pathlib.Path(args.kconfig).resolve()
    config_path = pathlib.Path(args.config).resolve()
    defconfig_path = pathlib.Path(args.defconfig).resolve()

    # kconfiglib 用 srctree 解析 source "..."；必须指向顶层 Kconfig 所在目录。
    os.environ["srctree"] = str(kconfig_path.parent)
    kconf = kconfiglib.Kconfig(str(kconfig_path))

    if config_path.exists():
        kconf.load_config(str(config_path))
    else:
        # 第一次运行：从模板初始化，并立刻落盘，保证菜单退出前异常也有配置。
        kconf.load_config(str(defconfig_path))
        config_path.parent.mkdir(parents=True, exist_ok=True)
        kconf.write_config(str(config_path))

    # 阻塞直到用户退出菜单；kconfiglib 内部会处理按键和终端尺寸。
    menuconfig.menuconfig(kconf)

    # 保存用户修改；fwconfig.py 随后会调用 genconfig 生成头文件。
    kconf.write_config(str(config_path))
    print(f"menuconfig: saved {config_path}")
    print("menuconfig: re-run cmake configure (or rebuild) to regenerate headers")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
