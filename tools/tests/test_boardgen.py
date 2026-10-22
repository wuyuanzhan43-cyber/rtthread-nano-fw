"""boardgen manifest/生成器单元测试（无硬件、无 kconfiglib）。"""

from __future__ import annotations

import copy
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import boardgen  # noqa: E402


def symbols(**overrides):
    """当前默认构建选择的最小符号集。"""
    values = {
        "FW_USING_UART1": ("bool", True),
        "FW_USING_UART2": ("bool", False),
        "FW_USING_UART3": ("bool", False),
        "FW_USING_SPI1": ("bool", True),
        "FW_USING_SPI2": ("bool", False),
        "FW_USING_SPI3": ("bool", False),
        "BOARD_SPI1_USE_MISO": ("bool", False),
        "BOARD_SPI2_USE_MISO": ("bool", True),
        "BOARD_SPI3_USE_MISO": ("bool", True),
        "BOARD_LED_ENABLE": ("bool", True),
        "BOARD_LCD_ENABLE": ("bool", True),
        "BOARD_UART_IRQ_PRIORITY": ("int", "6"),
        "BOARD_DMA_IRQ_PRIORITY": ("int", "7"),
        "BOARD_EXTI_IRQ_PRIORITY": ("int", "8"),
        "BOARD_MIN_STACK_SIZE": ("int", "4096"),
    }
    values.update(overrides)
    return values


class BoardgenTest(unittest.TestCase):
    def setUp(self):
        self.loaded = boardgen.load_profile(ROOT / "profiles" / "f407ve")
        self.hse = boardgen.load_profile(ROOT / "profiles" / "f407ve-hse")

    def test_clock_variants(self):
        hsi = boardgen.analyze(self.loaded, symbols())
        self.assertEqual(hsi["errors"], [])
        self.assertEqual(hsi["clock"]["sysclk"], 16000000)
        self.assertEqual(hsi["clock"]["pclk1"], 16000000)

        hse = boardgen.analyze(self.hse, symbols())
        self.assertEqual(hse["errors"], [])
        self.assertEqual(hse["clock"]["sysclk"], 168000000)
        self.assertEqual(hse["clock"]["hclk"], 168000000)
        self.assertEqual(hse["clock"]["pclk1"], 42000000)
        self.assertEqual(hse["clock"]["pclk2"], 84000000)

    def test_compat_symbols(self):
        out = boardgen.build_symbols(self.loaded, symbols())
        self.assertEqual(out["BOARD_NAME"][1], "STM32F407VE")
        self.assertEqual(out["BOARD_UART1_TX_PIN"][1], "9")
        self.assertEqual(out["BOARD_UART1_RX_PIN"][1], "10")
        self.assertEqual(out["BOARD_LED_PIN"][1], "2")
        self.assertEqual(out["BOARD_LCD_CS_PIN"][1], "4")
        self.assertEqual(out["BOARD_LCD_SPI_INSTANCE"][1], "1")
        self.assertEqual(len(out["BOARD_MANIFEST_HASH"][1]), 64)

    def test_duplicate_pin_rejected(self):
        loaded = copy.deepcopy(self.loaded)
        loaded["board"]["peripherals"]["uart"][0]["pins"]["tx"]["pin"] = "A10"
        result = boardgen.analyze(loaded, symbols())
        self.assertTrue(any("already used" in error for error in result["errors"]))

    def test_invalid_af_rejected(self):
        loaded = copy.deepcopy(self.loaded)
        loaded["board"]["peripherals"]["uart"][0]["pins"]["tx"]["af"] = 9
        result = boardgen.analyze(loaded, symbols())
        self.assertTrue(any("not a valid USART1_TX" in error
                            for error in result["errors"]))

    def test_dma_conflict_rejected(self):
        loaded = copy.deepcopy(self.loaded)
        options = symbols(**{
            "FW_USING_UART1_DMA_RX": ("bool", True),
            "FW_USING_UART1_DMA_TX": ("bool", True),
        })
        requests = loaded["chip"]["dma"]["requests"]
        requests["USART1_RX"] = [{"controller": 2, "stream": 7, "channel": 4}]
        requests["USART1_TX"] = [{"controller": 2, "stream": 7, "channel": 4}]
        result = boardgen.analyze(loaded, options)
        self.assertTrue(any("no free DMA stream for USART1_TX" in error
                            for error in result["errors"]))

    def test_clock_mismatch_rejected(self):
        loaded = copy.deepcopy(self.loaded)
        loaded["board"]["clock_variants"]["hsi16"]["sysclk_hz"] = 168000000
        result = boardgen.analyze(loaded, symbols())
        self.assertTrue(any("declares" in error for error in result["errors"]))

    def test_generated_files(self):
        analysis = boardgen.analyze(self.loaded, symbols())
        memory = boardgen.render_memory_ld(self.loaded, symbols())
        self.assertIn("BOARD_CODE_FLASH_LENGTH = 512K;", memory)
        self.assertIn("BOARD_MIN_STACK_SIZE = 4096;", memory)
        extra = boardgen.generate_extra(self.loaded, symbols(), analysis)
        self.assertIn("board_desc.h", extra)
        self.assertIn("board_summary.txt", extra)
        self.assertIn("board_keil.json", extra)
        self.assertIn("BOARD_DESC_MANIFEST_HASH", extra["board_desc.h"])

    def test_uart_dma_routes(self):
        """启用 UART1 DMA 后 route 应被分配并写入兼容宏。"""
        options = symbols(**{
            "FW_USING_UART1_DMA_RX": ("bool", True),
            "FW_USING_UART1_DMA_TX": ("bool", True),
        })
        result = boardgen.analyze(self.loaded, options)
        self.assertEqual(result["errors"], [])
        self.assertIn("USART1_RX", result["routes"])
        self.assertIn("USART1_TX", result["routes"])
        out = boardgen.build_symbols(self.loaded, options, result)
        self.assertNotEqual(out["BOARD_UART1_DMA_RX_CONTROLLER"][1], "255")
        self.assertNotEqual(out["BOARD_UART1_DMA_TX_STREAM"][1], "255")
        self.assertTrue(out["BOARD_UART1_DMA_RX_ENABLED"][1])

    def test_dma_disabled_has_no_routes(self):
        """默认配置不分配 UART DMA route。"""
        result = boardgen.analyze(self.loaded, symbols())
        self.assertEqual(result["errors"], [])
        self.assertNotIn("USART1_RX", result["routes"])
        self.assertNotIn("USART1_TX", result["routes"])

    def test_invalid_pin_text(self):
        with self.assertRaises(ValueError):
            boardgen.parse_pin("Z99")


if __name__ == "__main__":
    unittest.main()
