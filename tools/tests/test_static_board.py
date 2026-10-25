#!/usr/bin/env python3
"""静态板级事实源一致性测试。

这个测试不编译固件，只解析文本，作用是在 CI/CTest 中拦住“同一个
板级事实被改了两处但只改了一处”的漂移。板级事实现在有三个消费者：

    1. 手写 board_config.h（引脚/通道/内存/NVM 的唯一来源）；
    2. link.lds（GCC 链接脚本的内存区域）；
    3. keil/*.uvprojx、*.sct、startup_stm32f407xx.s（AC5 工程）。

它们格式不同、由不同工具消费，无法共享同一个宏，因此必须用测试
交叉校验。检查内容：

    - board_config.h 的 4 个内存常量与 link.lds 的 MEMORY 区域一致；
    - 代码区 + NVM = 总 Flash，且 NVM 紧接代码区；
    - link.lds 的 _Min_Stack_Size 与 BOARD_MIN_STACK_SIZE 一致；
    - Keil 启动文件的 Stack_Size 与 BOARD_MIN_STACK_SIZE 一致；
    - Keil uvprojx 的 IROM/IRAM、sct 的 LR_IROM1 与板级常量一致；
    - HSI/HSE 工程的 CLOCK 分别为 16MHz/168MHz；
    - profiles/*/generated/ 不再出现 boardgen 时代的旧生成物；
    - board_config.h 只包含 C 标准头和 board_policy.h；
    - 板级目录只保留单文件组合根所需的 6 个文件。

不检查：AF 是否属于某外设、DMA 请求映射、电气时序——这些需要
数据手册和上板 HIL。
"""

from __future__ import annotations

import pathlib
import re
import unittest

# 项目根目录：tools/tests/test_static_board.py -> parents[2]。
ROOT = pathlib.Path(__file__).resolve().parents[2]
BOARD_DIR = ROOT / "src" / "platform" / "board" / "stm32f407ve"
BOARD_CONFIG = BOARD_DIR / "board_config.h"
LINKER = BOARD_DIR / "link.lds"
STARTUP = ROOT / "keil" / "startup_stm32f407xx.s"

# boardgen 时代的生成物。它们曾经位于 profiles/*/generated/，现在由
# genconfig.py sync 自动删除；如果重新出现，说明旧生成链回归。
OBSOLETE = (
    "board_config.h",
    "board_desc.h",
    "board_memory.ld",
    "board_keil.json",
    "board_summary.txt",
    "board_pinout.md",
    "board_channels.md",
)


def header_defines(path: pathlib.Path) -> dict[str, int]:
    """读取头文件中的一行式整型 #define。

    只匹配：

        #define NAME 123
        #define NAME 0x1234
        #define NAME 123U
        #define NAME 0x1234UL   // 可选行尾注释

    不匹配带运算符的宏（例如 BOARD_NVM_SLOT_KB 的除法表达式）。
    这是有意为之：gen_keil.py 也只用同样的正则读取内存/Keil 常量，
    测试必须模拟生成器的解析能力，而不是比它更聪明。
    """
    result: dict[str, int] = {}
    text = path.read_text(encoding="utf-8")
    for match in re.finditer(
            r"^#define\s+([A-Z0-9_]+)\s+"
            r"(0x[0-9A-Fa-f]+|\d+)[uUlL]*\s*(?:/\*.*)?$",
            text, flags=re.M):
        result[match.group(1)] = int(match.group(2), 0)
    return result


def memory_regions(path: pathlib.Path) -> dict[str, tuple[int, int]]:
    """解析 link.lds 的 MEMORY 区域。

    支持的行格式（本项目统一使用 K 后缀）：

        FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 256K

    返回 {region_name: (origin_bytes, length_bytes)}。

    只解析 MEMORY 块中的声明；SECTIONS 里的用法不会匹配，因为
    正则要求行首是区域名且后面紧跟括号和冒号。
    """
    text = path.read_text(encoding="utf-8")
    region = {}
    for match in re.finditer(
            r"^\s*([A-Z_]+)\s*\([^)]*\)\s*:\s*"
            r"ORIGIN\s*=\s*(0x[0-9A-Fa-f]+|\d+)\s*,\s*"
            r"LENGTH\s*=\s*(\d+)\s*K",
            text, flags=re.M):
        region[match.group(1)] = (int(match.group(2), 0),
                                  int(match.group(3)) * 1024)
    return region


class BoardFactsTest(unittest.TestCase):
    """板级事实源的一致性断言集合。"""

    @classmethod
    def setUpClass(cls) -> None:
        """一次解析，供所有测试方法复用。"""
        cls.board = header_defines(BOARD_CONFIG)
        cls.regions = memory_regions(LINKER)

    def test_linker_matches_board_config(self):
        """GCC 链接脚本的 4 个区域必须来自同一份板级常量。

        FLASH 的长度是“代码区”，不是芯片总 Flash：NVM 从
        0x08040000 开始，不能被固件链接进去。
        """
        code_size = self.board["BOARD_CODE_FLASH_SIZE_KB"] * 1024
        self.assertEqual(self.regions["FLASH"],
                         (self.board["BOARD_FLASH_ORIGIN"], code_size))
        self.assertEqual(self.regions["NVM"],
                         (self.board["BOARD_NVM_ORIGIN"],
                          self.board["BOARD_NVM_SIZE_KB"] * 1024))
        self.assertEqual(self.regions["RAM"],
                         (self.board["BOARD_RAM_ORIGIN"],
                          self.board["BOARD_RAM_SIZE_KB"] * 1024))
        self.assertEqual(self.regions["CCM"],
                         (self.board["BOARD_CCM_ORIGIN"],
                          self.board["BOARD_CCM_SIZE_KB"] * 1024))

    def test_nvm_partition(self):
        """代码区 + NVM 必须正好覆盖总 Flash，且 NVM 紧接代码区。

        这三条关系同时保证了：
          - 固件不会写进 NVM；
          - NVM 不越过芯片 Flash 末尾；
          - A/B 两个槽各占一个可独立擦除的扇区。
        """
        self.assertEqual(
            self.board["BOARD_CODE_FLASH_SIZE_KB"]
            + self.board["BOARD_NVM_SIZE_KB"],
            self.board["BOARD_FLASH_SIZE_KB"])
        self.assertEqual(
            self.board["BOARD_NVM_ORIGIN"],
            self.board["BOARD_FLASH_ORIGIN"]
            + self.board["BOARD_CODE_FLASH_SIZE_KB"] * 1024)
        # A/B 两个槽，每个槽必须是一个可独立擦除的扇区。
        self.assertEqual(
            self.board["BOARD_NVM_SIZE_KB"],
            2 * self.board["BOARD_NVM_SECTOR_KB"])

    def test_linker_stack(self):
        """link.lds 的 _Min_Stack_Size 必须等于 BOARD_MIN_STACK_SIZE。

        link.lds 不能 include 生成的 memory 脚本了，因此这行是手写的；
        测试防止改板级常量时漏改链接脚本。
        """
        text = LINKER.read_text(encoding="utf-8")
        match = re.search(r"_Min_Stack_Size\s*=\s*(\d+)\s*;", text)
        self.assertIsNotNone(match)
        self.assertEqual(int(match.group(1)),
                         self.board["BOARD_MIN_STACK_SIZE"])

    def test_keil_startup_stack(self):
        """Keil 启动文件的 Stack_Size 必须等于 BOARD_MIN_STACK_SIZE。

        gen_keil.py 会重写这一行；如果用户手工改过启动文件或忘记
        重新生成，这里会失败。
        """
        raw = STARTUP.read_text(encoding="utf-8", errors="ignore")
        match = re.search(r"Stack_Size\s+EQU\s+(0x[0-9A-Fa-f]+|\d+)", raw)
        self.assertIsNotNone(match)
        self.assertEqual(int(match.group(1), 0),
                         self.board["BOARD_MIN_STACK_SIZE"])

    def test_keil_memory_and_clock(self):
        """Keil 工程的 IROM/IRAM/CLOCK 与板级常量一致。

        为什么重要：
          - uvprojx 的 IROM 决定下载/擦除范围，必须是代码区 256K，
            否则烧录会擦掉 NVM 参数区；
          - sct 的 LR_IROM1 决定链接器可用地址空间；
          - CLOCK 只影响 uVision 的调试显示/器件校验，但 HSE profile
            必须显示 168MHz，否则说明时钟源选择没有生效。
        """
        code_size = self.board["BOARD_CODE_FLASH_SIZE_KB"] * 1024
        ram_size = self.board["BOARD_RAM_SIZE_KB"] * 1024
        irom = f"IROM(0x{self.board['BOARD_FLASH_ORIGIN']:08X},0x{code_size:08X})"
        iram = f"IRAM(0x{self.board['BOARD_RAM_ORIGIN']:08X},0x{ram_size:08X})"
        for name in ("stm32f407ve", "stm32f407ve-hse"):
            project = (ROOT / "keil" / f"{name}.uvprojx").read_text(
                encoding="utf-8-sig")
            self.assertIn(irom, project)
            self.assertIn(iram, project)
            scatter = (ROOT / "keil" / f"{name}.sct").read_text(
                encoding="utf-8-sig")
            self.assertIn(f"LR_IROM1 0x{self.board['BOARD_FLASH_ORIGIN']:08X} "
                          f"0x{code_size:08X}", scatter)

        hsi = (ROOT / "keil" / "stm32f407ve.uvprojx").read_text(
            encoding="utf-8-sig")
        hse = (ROOT / "keil" / "stm32f407ve-hse.uvprojx").read_text(
            encoding="utf-8-sig")
        self.assertIn("CLOCK(16000000)", hsi)
        self.assertIn("CLOCK(168000000)", hse)

    def test_no_generated_board_outputs(self):
        """profiles/*/generated/ 不能再出现 boardgen 时代的旧文件。

        generated/ 在 include 路径最前；如果残留旧 board_config.h，
        它会遮蔽手写 board_config.h，造成“代码看起来改了但固件没变”
        的严重问题。genconfig.py sync 会自动删除，这里做最后防线。
        """
        for profile in (ROOT / "profiles").iterdir():
            generated = profile / "generated"
            if not generated.is_dir():
                continue
            for name in OBSOLETE:
                self.assertFalse((generated / name).exists(),
                                 f"{profile.name}/generated/{name} is obsolete")

    def test_board_config_is_pure_c(self):
        """board_config.h 只能包含 C 标准头与生成的策略头。

        这是 Host 表测试和“板级头不泄漏 HAL 类型”的前提。允许集合
        是精确的：新增头文件必须先想清楚它是否把平台依赖带进板级
        事实源。
        """
        text = BOARD_CONFIG.read_text(encoding="utf-8")
        includes = re.findall(r'^#include\s+(?:"([^"]+)"|<([^>]+)>)',
                              text, flags=re.M)
        names = {quoted or angled for quoted, angled in includes}
        self.assertEqual(names, {"stdbool.h", "stdint.h", "board_policy.h"})

    def test_single_board_module(self):
        """板级目录只保留组合根、公共头和接线事实源。

        这是本重构的核心约束：不再有 board_devices.c、board_pins.c、
        board_clock.c 等拆分文件。新增文件必须同步更新本测试，迫使
        作者重新审视“是否真的需要新文件”。
        """
        expected = {
            "board.c", "board.h", "board_config.h",
            "Kconfig.board", "link.lds", "BOARD.md",
        }
        actual = {path.name for path in BOARD_DIR.iterdir() if path.is_file()}
        self.assertEqual(actual, expected)


if __name__ == "__main__":
    unittest.main()
