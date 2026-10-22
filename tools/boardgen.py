#!/usr/bin/env python3
"""Board/chip manifest 加载、校验与板级生成。

设计边界：
    - manifest 是引脚/AF/时钟/内存/设备绑定的唯一事实源；
    - Kconfig 只提供启用项、容量、IRQ 优先级等构建选择；
    - 本模块不依赖 kconfiglib，也不写 profile/generated 目录；
    - genconfig.py 负责把它合并进生成流水线。

manifest 路径：
    chips/<chip-id>.json
    boards/<board-id>/board.json
    profiles/<profile>/profile.json
"""

from __future__ import annotations

import hashlib
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PIN_RE = re.compile(r"^([A-Z])([0-9]{1,2})$")
HEX_OR_DEC_RE = re.compile(r"^(0x[0-9A-Fa-f]+|[0-9]+)$")


def load_json(path: Path) -> dict:
    """读取 JSON manifest；失败时抛出带路径的 SystemExit。"""
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        raise SystemExit(f"boardgen: missing {path.relative_to(ROOT)}")
    except json.JSONDecodeError as error:
        raise SystemExit(f"boardgen: invalid JSON in {path.relative_to(ROOT)}: {error}")


def load_profile(profile_dir: Path) -> dict:
    """加载 profiles/<name>/profile.json 及其引用的 chip/board manifest。"""
    profile_dir = Path(profile_dir)
    profile = load_json(profile_dir / "profile.json")
    if profile.get("schema_version") != 1:
        raise SystemExit(f"boardgen: unsupported profile schema in {profile_dir}")
    platform = profile.get("platform")
    if platform == "host":
        return {"platform": "host", "profile": profile}

    chip_path = profile.get("chip")
    board_path = profile.get("board")
    if not chip_path or not board_path:
        raise SystemExit(f"boardgen: profile {profile_dir} needs chip and board")
    chip = load_json(ROOT / chip_path)
    board = load_json(ROOT / board_path)
    variant = profile.get("clock_variant") or board.get("default_clock_variant")
    return {
        "platform": platform,
        "profile": profile,
        "chip": chip,
        "board": board,
        "variant": variant,
        "chip_path": chip_path,
        "board_path": board_path,
    }


def parse_pin(text: str) -> tuple[int, int]:
    """把 A9/PE2 解析成 (port_index, pin_number)。"""
    match = PIN_RE.match(str(text))
    if match is None:
        raise ValueError(f"invalid pin {text!r}")
    port = ord(match.group(1)) - ord("A")
    pin = int(match.group(2))
    if port < 0 or pin < 0 or pin > 15:
        raise ValueError(f"invalid pin {text!r}")
    return port, pin


def pin_number(text: str) -> int:
    port, pin = parse_pin(text)
    return (port << 4) | pin


def _sym_bool(symbols: dict, name: str, default: bool = False) -> bool:
    item = symbols.get(name)
    return bool(item[1]) if item is not None else default


def _sym_int(symbols: dict, name: str, default: int = 0) -> int:
    item = symbols.get(name)
    if item is None:
        return default
    try:
        return int(item[1], 0)
    except (TypeError, ValueError):
        return default


def _add(errors: list[str], condition: bool, message: str) -> None:
    if not condition:
        errors.append(message)


def _ports(chip: dict) -> dict[str, int]:
    return {name: index for index, name in enumerate(chip["package"]["ports"])}


def _find_named(entries: list[dict], name: str) -> dict | None:
    for entry in entries:
        if entry.get("name") == name:
            return entry
    return None


def _find_instance(entries: list[dict], instance: int) -> dict | None:
    for entry in entries:
        if entry.get("instance") == instance:
            return entry
    return None


def _reserved_pins(chip: dict, board: dict) -> set[str]:
    package = chip["package"]
    reserved = set(package.get("reserved_pins", []))
    reserved.update(package.get("boot_pins", []))
    reserved.update(board.get("reserved_pins", []))
    return reserved


def _ram_extent(chip: dict, errors: list[str]) -> tuple[str, int]:
    """返回连续主 SRAM 的起始地址和总大小（KB）。"""
    regions = chip["memory"]["sram"]
    if not regions:
        errors.append("chip has no sram regions")
        return "0x20000000", 0
    origin = int(regions[0]["origin"], 0)
    total = 0
    expected = origin
    for region in regions:
        start = int(region["origin"], 0)
        size = int(region["size_kb"])
        if start != expected:
            errors.append("sram regions are not contiguous; boardgen needs a contiguous extent")
        expected = start + size * 1024
        total += size
    return f"0x{origin:08X}", total


def compute_clock(chip: dict, board: dict, variant_name: str,
                  errors: list[str]) -> dict:
    """校验并计算所选 clock variant 的 HCLK/PCLK。"""
    variants = board.get("clock_variants", {})
    variant = variants.get(variant_name)
    if variant is None:
        errors.append(f"clock variant {variant_name!r} is not defined by board")
        return {}

    clk = chip["clock"]
    source = variant.get("source")
    if source == "hsi":
        sysclk = int(clk["hsi_hz"])
    elif source == "hse":
        hse = int(variant.get("hse_hz", 0))
        hse_min, hse_max = clk["hse_range_hz"]
        if not hse_min <= hse <= hse_max:
            errors.append(f"HSE {hse} outside [{hse_min}, {hse_max}]")
        pll = variant.get("pll", {})
        ranges = clk["pll"]
        m = int(pll.get("m", 0))
        n = int(pll.get("n", 0))
        p = int(pll.get("p", 0))
        q = int(pll.get("q", 0))
        if not ranges["m"][0] <= m <= ranges["m"][1]:
            errors.append(f"PLLM {m} outside {ranges['m']}")
        if not ranges["n"][0] <= n <= ranges["n"][1]:
            errors.append(f"PLLN {n} outside {ranges['n']}")
        if p not in ranges["p"]:
            errors.append(f"PLLP {p} outside {ranges['p']}")
        if not ranges["q"][0] <= q <= ranges["q"][1]:
            errors.append(f"PLLQ {q} outside {ranges['q']}")
        if m <= 0 or p <= 0:
            errors.append("PLL M/P must be positive")
            return {}
        if hse % m != 0:
            errors.append(f"HSE {hse} is not divisible by PLLM {m}")
        vco = (hse // m) * n
        if vco % p != 0:
            errors.append(f"VCO {vco} is not divisible by PLLP {p}")
        sysclk = vco // p
    else:
        errors.append(f"unsupported clock source {source!r}")
        return {}

    declared = int(variant.get("sysclk_hz", 0))
    if declared != sysclk:
        errors.append(f"clock variant declares {declared} Hz but computes {sysclk} Hz")
    if sysclk > int(clk["max_sysclk_hz"]):
        errors.append(f"SYSCLK {sysclk} exceeds {clk['max_sysclk_hz']}")

    ahb = int(variant.get("ahb_div", 1))
    apb1 = int(variant.get("apb1_div", 1))
    apb2 = int(variant.get("apb2_div", 1))
    if ahb not in (1, 2, 4, 8, 16, 64, 128, 256, 512):
        errors.append(f"invalid AHB divider {ahb}")
    if apb1 not in (1, 2, 4, 8, 16) or apb2 not in (1, 2, 4, 8, 16):
        errors.append("invalid APB divider")
    if ahb <= 0 or apb1 <= 0 or apb2 <= 0:
        return {}
    if sysclk % ahb != 0:
        errors.append(f"SYSCLK {sysclk} not divisible by AHB {ahb}")
    hclk = sysclk // ahb
    if hclk % apb1 != 0 or hclk % apb2 != 0:
        errors.append("HCLK not divisible by APB dividers")
    pclk1 = hclk // apb1
    pclk2 = hclk // apb2
    if pclk1 > int(clk["apb1_max_hz"]):
        errors.append(f"PCLK1 {pclk1} exceeds {clk['apb1_max_hz']}")
    if pclk2 > int(clk["apb2_max_hz"]):
        errors.append(f"PCLK2 {pclk2} exceeds {clk['apb2_max_hz']}")

    required_latency = None
    for row in sorted(clk["flash_latency"], key=lambda item: item["max_hz"]):
        if sysclk <= row["max_hz"]:
            required_latency = row["latency"]
            break
    if required_latency is None:
        errors.append(f"no flash latency for {sysclk} Hz")
    else:
        declared_latency = int(variant.get("flash_latency", -1))
        if declared_latency != required_latency:
            errors.append(
                f"flash latency {declared_latency} does not match {required_latency} "
                f"for {sysclk} Hz")

    return {
        "variant": variant_name,
        "source": source,
        "sysclk": sysclk,
        "hclk": hclk,
        "pclk1": pclk1,
        "pclk2": pclk2,
        "flash_latency": int(variant.get("flash_latency", 0)),
        "hse_hz": int(variant.get("hse_hz", 0)),
    }


def allocate_dma(loaded: dict, symbols: dict, errors: list[str]) -> dict:
    """按固定优先级分配 UART/SPI 的 DMA stream。

    第一版只处理 UART/SPI；I2C/ADC/PWM 在后续里程碑按同样方式加入。
    路由结果键为 USART1_RX / SPI1_TX 等，供 board_desc.h 使用。
    """
    chip = loaded["chip"]
    board = loaded["board"]
    requests = chip["dma"]["requests"]
    owners: dict[tuple[int, int], str] = {}
    routes: dict[str, dict] = {}

    uarts = board["peripherals"].get("uart", [])
    spis = board["peripherals"].get("spi", [])
    console = board.get("devices", {}).get("console")
    lcd_spi = board.get("devices", {}).get("lcd", {}).get("spi")

    jobs: list[tuple[str, object, str]] = []
    for entry in sorted(uarts, key=lambda item: (item.get("name") != console,
                                                 item.get("instance", 0))):
        if not _sym_bool(symbols, f"FW_USING_UART{entry.get('instance')}"):
            continue
        dma = entry.get("dma", {})
        for signal in ("rx", "tx"):
            # DMA 是构建开关：未启用时即使 manifest 写了 auto 也不分配 stream。
            flag = f"FW_USING_UART{entry['instance']}_DMA_{signal.upper()}"
            if not _sym_bool(symbols, flag):
                continue
            spec = dma.get(signal)
            if spec in (None, "none"):
                errors.append(f"{flag}=y but board manifest has no DMA route")
                continue
            jobs.append((f"USART{entry['instance']}_{signal.upper()}", spec,
                         f"{entry.get('name', 'uart')}.{signal}"))

    for entry in sorted(spis, key=lambda item: (item.get("name") != lcd_spi,
                                                item.get("instance", 0))):
        instance = entry.get("instance")
        if not _sym_bool(symbols, f"FW_USING_SPI{instance}"):
            continue
        if not _sym_bool(symbols, f"FW_USING_SPI{instance}_DMA"):
            continue
        dma = entry.get("dma", {})
        for signal in ("rx", "tx"):
            if signal == "rx" and not _sym_bool(symbols, f"BOARD_SPI{instance}_USE_MISO"):
                continue
            spec = dma.get(signal)
            if spec in (None, "none"):
                errors.append(f"FW_USING_SPI{instance}_DMA=y but no DMA route in board manifest")
                continue
            jobs.append((f"SPI{instance}_{signal.upper()}", spec,
                         f"{entry.get('name', 'spi')}.{signal}"))

    for key, spec, owner in jobs:
        candidates = requests.get(key)
        if not candidates:
            errors.append(f"no DMA request mapping for {key} in chip manifest")
            continue
        chosen = None
        if spec == "auto":
            for candidate in candidates:
                slot = (int(candidate["controller"]), int(candidate["stream"]))
                if slot not in owners:
                    chosen = candidate
                    break
        elif isinstance(spec, dict):
            for candidate in candidates:
                if (int(spec.get("controller", -1)) == int(candidate["controller"]) and
                        int(spec.get("stream", -1)) == int(candidate["stream"]) and
                        int(spec.get("channel", -1)) == int(candidate["channel"])):
                    if (int(candidate["controller"]), int(candidate["stream"])) in owners:
                        errors.append(f"DMA stream for {key} already owned by "
                                      f"{owners[(int(candidate['controller']), int(candidate['stream']))]}")
                    else:
                        chosen = candidate
                    break
        else:
            errors.append(f"invalid DMA spec for {key}: {spec!r}")
            continue

        if chosen is None:
            if spec == "auto":
                errors.append(f"no free DMA stream for {key}; candidates="
                              f"{candidates}, owners={owners}")
            continue

        slot = (int(chosen["controller"]), int(chosen["stream"]))
        owners[slot] = owner
        routes[key] = {
            "controller": int(chosen["controller"]),
            "stream": int(chosen["stream"]),
            "channel": int(chosen["channel"]),
            "owner": owner,
        }

    return routes


def analyze(loaded: dict, symbols: dict) -> dict:
    """校验 manifest 与 Kconfig 的组合，返回错误、时钟、引脚和 DMA 结果。"""
    if loaded.get("platform") == "host":
        return {"errors": [], "clock": {}, "used": {}, "routes": {}, "reserved": set()}

    chip = loaded["chip"]
    board = loaded["board"]
    errors: list[str] = []
    used: dict[tuple[int, int], str] = {}
    ports = _ports(chip)
    reserved = _reserved_pins(chip, board)

    _add(errors, loaded.get("variant") in board.get("clock_variants", {}),
         f"clock variant {loaded.get('variant')!r} missing")
    clock = compute_clock(chip, board, loaded.get("variant"), errors)

    _add(errors, board.get("chip") == chip.get("id"),
         f"board.chip {board.get('chip')!r} does not match chip.id {chip.get('id')!r}")
    _add(errors, chip.get("schema_version") == 1, "unsupported chip schema_version")
    _add(errors, board.get("schema_version") == 1, "unsupported board schema_version")

    def verify_pin(label: str, pin_text: str, af: int | None,
                   function: str | None, reserve: bool) -> tuple[int, int] | None:
        try:
            port, pin = parse_pin(pin_text)
        except ValueError as error:
            errors.append(f"{label}: {error}")
            return None
        port_name = chr(ord("A") + port)
        if port_name not in ports:
            errors.append(f"{label}: port {port_name} is not available on {chip['id']}")
        if pin_text in reserved:
            errors.append(f"{label}: pin {pin_text} is reserved")
        if function is not None:
            candidates = chip.get("pinmux", {}).get(function)
            if not candidates:
                errors.append(f"{label}: function {function} is not defined by chip manifest")
            elif not any(item.get("pin") == pin_text and
                         (af is None or item.get("af") == af) for item in candidates):
                errors.append(f"{label}: {pin_text} AF{af} is not a valid {function}; "
                              f"candidates={candidates}")
        if reserve:
            key = (port, pin)
            if key in used:
                errors.append(f"{label}: pin {pin_text} already used by {used[key]}")
            else:
                used[key] = label
        return port, pin

    uarts = board.get("peripherals", {}).get("uart", [])
    spis = board.get("peripherals", {}).get("spi", [])
    devices = board.get("devices", {})
    enable_led = _sym_bool(symbols, "BOARD_LED_ENABLE", True)
    enable_lcd = _sym_bool(symbols, "BOARD_LCD_ENABLE", True)

    for number in (1, 2, 3):
        if _sym_bool(symbols, f"FW_USING_UART{number}") and \
                _find_instance(uarts, number) is None:
            errors.append(f"UART{number} is enabled but missing from board manifest")
        if _sym_bool(symbols, f"FW_USING_SPI{number}") and \
                _find_instance(spis, number) is None:
            errors.append(f"SPI{number} is enabled but missing from board manifest")

    for entry in uarts:
        instance = int(entry.get("instance", 0))
        enabled = _sym_bool(symbols, f"FW_USING_UART{instance}")
        pins = entry.get("pins", {})
        tx = pins.get("tx")
        rx = pins.get("rx")
        if not tx or not rx:
            errors.append(f"UART{instance}: need tx and rx pin descriptions")
            continue
        verify_pin(f"UART{instance}_TX", tx["pin"], tx.get("af"), f"USART{instance}_TX", enabled)
        verify_pin(f"UART{instance}_RX", rx["pin"], rx.get("af"), f"USART{instance}_RX", enabled)
        if int(tx.get("af", -1)) != int(rx.get("af", -2)):
            errors.append(f"UART{instance}: TX/RX AF differ; compatibility layer needs one AF")

    for entry in spis:
        instance = int(entry.get("instance", 0))
        enabled = _sym_bool(symbols, f"FW_USING_SPI{instance}")
        pins = entry.get("pins", {})
        sck = pins.get("sck")
        mosi = pins.get("mosi")
        miso = pins.get("miso")
        if not sck or not mosi:
            errors.append(f"SPI{instance}: need sck and mosi pin descriptions")
            continue
        use_miso = _sym_bool(symbols, f"BOARD_SPI{instance}_USE_MISO", False)
        verify_pin(f"SPI{instance}_SCK", sck["pin"], sck.get("af"), f"SPI{instance}_SCK", enabled)
        verify_pin(f"SPI{instance}_MOSI", mosi["pin"], mosi.get("af"), f"SPI{instance}_MOSI", enabled)
        if miso:
            verify_pin(f"SPI{instance}_MISO", miso["pin"], miso.get("af"),
                       f"SPI{instance}_MISO", enabled and use_miso)
            afs = {int(sck.get("af", -1)), int(mosi.get("af", -2)),
                   int(miso.get("af", -3))}
        else:
            afs = {int(sck.get("af", -1)), int(mosi.get("af", -2))}
        if len(afs) != 1:
            errors.append(f"SPI{instance}: signal AFs differ; compatibility layer needs one AF")

    named = board.get("named_pins", {})
    for name, item in named.items():
        try:
            parse_pin(item["pin"])
        except (KeyError, ValueError) as error:
            errors.append(f"named pin {name}: {error}")

    def named_pin(name: str, label: str, reserve: bool) -> dict | None:
        item = named.get(name)
        if item is None:
            errors.append(f"{label}: named pin {name!r} is missing")
            return None
        verify_pin(label, item["pin"], None, None, reserve)
        return item

    console_name = devices.get("console")
    console_entry = _find_named(uarts, console_name) if console_name else None
    if console_entry is None:
        errors.append(f"console {console_name!r} does not reference a declared UART")
    elif not _sym_bool(symbols, f"FW_USING_UART{console_entry.get('instance')}"):
        errors.append(f"console {console_name!r} refers to a disabled UART")

    led_name = devices.get("led")
    led_item = None
    if led_name:
        led_item = named.get(led_name)
        if led_item is None:
            errors.append(f"LED {led_name!r} is not a named pin")
        else:
            verify_pin("LED", led_item["pin"], None, None, enable_led)
    elif enable_led:
        errors.append("BOARD_LED_ENABLE=y but devices.led is not set")

    lcd = devices.get("lcd")
    if lcd is None and enable_lcd:
        errors.append("BOARD_LCD_ENABLE=y but devices.lcd is not set")
    if lcd is not None:
        spi_name = lcd.get("spi")
        spi_entry = _find_named(spis, spi_name) if spi_name else None
        if spi_entry is None:
            errors.append(f"LCD spi {spi_name!r} does not reference a declared SPI")
        elif not _sym_bool(symbols, f"FW_USING_SPI{spi_entry.get('instance')}"):
            errors.append(f"LCD spi {spi_name!r} refers to a disabled SPI")
        for key, label in (("cs", "LCD_CS"), ("dc", "LCD_DC"),
                           ("res", "LCD_RES"), ("bl", "LCD_BL")):
            pin_name = lcd.get("pins", {}).get(key)
            if not pin_name:
                errors.append(f"{label}: missing named pin binding")
                continue
            named_pin(pin_name, label, enable_lcd)

    routes = allocate_dma(loaded, symbols, errors)

    # IRQ 优先级属于构建/运行策略，保留在 Kconfig；manifest 不复制。
    irq = {
        "uart": _sym_int(symbols, "BOARD_UART_IRQ_PRIORITY", 6),
        "dma": _sym_int(symbols, "BOARD_DMA_IRQ_PRIORITY", 7),
        "exti": _sym_int(symbols, "BOARD_EXTI_IRQ_PRIORITY", 8),
        "systick": 255,
    }
    for name in ("uart", "dma", "exti"):
        value = int(irq[name])
        if not 0 <= value <= 15:
            errors.append(f"board.irq.{name} must be 0..15, got {value}")
    if not (irq["uart"] < irq["dma"] < irq["exti"] < irq["systick"]):
        errors.append("IRQ priority must satisfy uart < dma < exti < 255")

    memory = chip["memory"]
    flash_origin = int(memory["flash"]["origin"], 0)
    flash_size = int(memory["flash"]["size_kb"]) * 1024
    nvm = board.get("peripherals", {}).get("nvm")
    if nvm:
        nvm_origin = int(nvm["origin"], 0)
        nvm_size = int(nvm["size_kb"]) * 1024
        sector = int(nvm.get("sector_kb", 0)) * 1024
        if nvm_origin < flash_origin or nvm_origin + nvm_size > flash_origin + flash_size:
            errors.append("NVM partition is outside flash")
        if sector and (nvm_origin % sector != 0 or nvm_size % sector != 0):
            errors.append("NVM partition must be sector aligned")
        if nvm_origin < flash_origin + flash_size - nvm_size:
            errors.append("NVM partition must occupy the tail of flash")

    return {
        "errors": errors,
        "clock": clock,
        "used": used,
        "routes": routes,
        "reserved": reserved,
        "irq": irq,
        "led": led_item,
        "lcd": lcd,
    }


def manifest_hash(loaded: dict) -> str:
    """对 chip/board/clock variant 做确定性哈希。"""
    payload = {
        "chip": loaded["chip"],
        "board": loaded["board"],
        "clock_variant": loaded.get("variant"),
    }
    canonical = json.dumps(payload, sort_keys=True, separators=(",", ":"),
                           ensure_ascii=True)
    return hashlib.sha256(canonical.encode("utf-8")).hexdigest()


def build_symbols(loaded: dict, symbols: dict, analysis: dict | None = None) -> dict:
    """从 manifest 生成与旧 BOARD_* 兼容的符号字典。

    analysis 提供 DMA 路由分配结果；未传时只生成静态拓扑宏。
    DMA 路由宏统一输出（未分配为 0xFF），这样 board_pins.c 可以用同一套
    宏填描述符，不需要知道 manifest 的内部结构。
    """
    if loaded.get("platform") == "host":
        return {}

    chip = loaded["chip"]
    board = loaded["board"]
    clock = compute_clock(chip, board, loaded.get("variant"), [])
    routes = analysis.get("routes", {}) if analysis else {}
    out: dict[str, tuple] = {}

    def add(name: str, kind: str, value) -> None:
        out[name] = (kind, value)

    identity = board.get("identity", {})
    add("BOARD_NAME", "string", identity.get("name", board.get("id", "UNKNOWN")))
    add("BOARD_VERSION", "string", identity.get("version", "0.0.0"))
    add("BOARD_MANIFEST_HASH", "string", manifest_hash(loaded))

    memory = chip["memory"]
    flash = memory["flash"]
    add("BOARD_FLASH_ORIGIN", "hex", str(flash["origin"]))
    add("BOARD_FLASH_SIZE_KB", "int", str(int(flash["size_kb"])))
    ram_origin, ram_size = _ram_extent(chip, [])
    add("BOARD_RAM_ORIGIN", "hex", ram_origin)
    add("BOARD_RAM_SIZE_KB", "int", str(ram_size))
    ccm = memory.get("ccm")
    if ccm:
        add("BOARD_CCM_ORIGIN", "hex", str(ccm["origin"]))
        add("BOARD_CCM_SIZE_KB", "int", str(int(ccm["size_kb"])))

    source = clock.get("source", board["clock_variants"][loaded["variant"]].get("source"))
    add("BOARD_CLOCK_HSI_16", "bool", source == "hsi")
    add("BOARD_CLOCK_HSE_8_168", "bool", source == "hse")
    add("BOARD_HSE_VALUE", "int", str(int(clock.get("hse_hz", 0))))

    named = board.get("named_pins", {})
    devices = board.get("devices", {})
    led_name = devices.get("led")
    led = named.get(led_name) if led_name else None
    if led:
        port, pin = parse_pin(led["pin"])
        add("BOARD_LED_PORT", "int", str(port))
        add("BOARD_LED_PIN", "int", str(pin))
        add("BOARD_LED_ACTIVE_HIGH", "bool", bool(led.get("active_high", True)))

    console_name = devices.get("console")
    console_entry = _find_named(board["peripherals"].get("uart", []), console_name)
    console_num = int(console_entry.get("instance", 0)) if console_entry else 0
    add("BOARD_CONSOLE_UART_NUM", "int", str(console_num))
    for number in (1, 2, 3):
        add(f"BOARD_CONSOLE_UART{number}", "bool", console_num == number)

    for entry in board["peripherals"].get("uart", []):
        number = int(entry["instance"])
        for signal in ("tx", "rx"):
            pin = entry["pins"][signal]
            port, number_pin = parse_pin(pin["pin"])
            add(f"BOARD_UART{number}_{signal.upper()}_PORT", "int", str(port))
            add(f"BOARD_UART{number}_{signal.upper()}_PIN", "int", str(number_pin))
        add(f"BOARD_UART{number}_AF", "int", str(int(entry["pins"]["tx"]["af"])))
        for signal in ("rx", "tx"):
            key = f"USART{number}_{signal.upper()}"
            route = routes.get(key)
            add(f"BOARD_UART{number}_DMA_{signal.upper()}_CONTROLLER", "int",
                str(route["controller"] if route else 0xFF))
            add(f"BOARD_UART{number}_DMA_{signal.upper()}_STREAM", "int",
                str(route["stream"] if route else 0xFF))
            add(f"BOARD_UART{number}_DMA_{signal.upper()}_CHANNEL", "int",
                str(route["channel"] if route else 0xFF))
            add(f"BOARD_UART{number}_DMA_{signal.upper()}_ENABLED", "bool",
                _sym_bool(symbols, f"FW_USING_UART{number}_DMA_{signal.upper()}"))

    for entry in board["peripherals"].get("spi", []):
        number = int(entry["instance"])
        for signal in ("sck", "mosi", "miso"):
            pin = entry["pins"].get(signal)
            if not pin:
                continue
            port, number_pin = parse_pin(pin["pin"])
            add(f"BOARD_SPI{number}_{signal.upper()}_PORT", "int", str(port))
            add(f"BOARD_SPI{number}_{signal.upper()}_PIN", "int", str(number_pin))
        add(f"BOARD_SPI{number}_AF", "int", str(int(entry["pins"]["sck"]["af"])))
        for signal in ("rx", "tx"):
            key = f"SPI{number}_{signal.upper()}"
            route = routes.get(key)
            add(f"BOARD_SPI{number}_DMA_{signal.upper()}_CONTROLLER", "int",
                str(route["controller"] if route else 0xFF))
            add(f"BOARD_SPI{number}_DMA_{signal.upper()}_STREAM", "int",
                str(route["stream"] if route else 0xFF))
            add(f"BOARD_SPI{number}_DMA_{signal.upper()}_CHANNEL", "int",
                str(route["channel"] if route else 0xFF))
        add(f"BOARD_SPI{number}_DMA_ENABLED", "bool",
            _sym_bool(symbols, f"FW_USING_SPI{number}_DMA"))

    lcd = devices.get("lcd")
    if lcd:
        spi_entry = _find_named(board["peripherals"].get("spi", []), lcd.get("spi"))
        add("BOARD_LCD_SPI_INSTANCE", "int",
            str(int(spi_entry["instance"]) if spi_entry else 0))
        for key, macro in (("cs", "CS"), ("dc", "DC"), ("res", "RES"), ("bl", "BL")):
            pin_name = lcd.get("pins", {}).get(key)
            pin = named.get(pin_name)
            if pin:
                port, number_pin = parse_pin(pin["pin"])
                add(f"BOARD_LCD_{macro}_PORT", "int", str(port))
                add(f"BOARD_LCD_{macro}_PIN", "int", str(number_pin))
        bl_pin = named.get(lcd.get("pins", {}).get("bl"), {})
        add("BOARD_LCD_BL_ACTIVE_HIGH", "bool", bool(bl_pin.get("active_high", True)))
        add("BOARD_LCD_WIDTH", "int", str(int(lcd.get("width", 0))))
        add("BOARD_LCD_HEIGHT", "int", str(int(lcd.get("height", 0))))
        add("BOARD_LCD_X_OFFSET", "int", str(int(lcd.get("x_offset", 0))))
        add("BOARD_LCD_Y_OFFSET", "int", str(int(lcd.get("y_offset", 0))))
        add("BOARD_LCD_MADCTL", "hex", str(lcd.get("madctl", "0x00")))
        add("BOARD_LCD_INVERT", "bool", bool(lcd.get("invert", False)))

    return out


def _hex32(value: int) -> str:
    return f"0x{value:08X}"


def _pin_letter(port: int) -> str:
    return chr(ord("A") + port)


def render_board_desc(loaded: dict, analysis: dict) -> str:
    """生成 board_desc.h：身份、时钟、命名引脚、DMA 路由。"""
    chip = loaded["chip"]
    board = loaded["board"]
    clock = analysis.get("clock", {})
    routes = analysis.get("routes", {})
    digest = manifest_hash(loaded)
    lines = [
        "/*",
        " * board_desc.h - generated by tools/boardgen.py. Do not edit.",
        " * Board identity, clock, named pins and DMA routes.",
        " */",
        "#ifndef FW_GENERATED_BOARD_DESC_H",
        "#define FW_GENERATED_BOARD_DESC_H",
        "",
        f'#define BOARD_DESC_ID "{board.get("id", "unknown")}"',
        f'#define BOARD_DESC_CHIP "{chip.get("id", "unknown")}"',
        f'#define BOARD_DESC_CLOCK_VARIANT "{clock.get("variant", "")}"',
        f'#define BOARD_DESC_MANIFEST_HASH "{digest}"',
        f'#define BOARD_DESC_SYSCLK_HZ {int(clock.get("sysclk", 0))}U',
        f'#define BOARD_DESC_HCLK_HZ {int(clock.get("hclk", 0))}U',
        f'#define BOARD_DESC_PCLK1_HZ {int(clock.get("pclk1", 0))}U',
        f'#define BOARD_DESC_PCLK2_HZ {int(clock.get("pclk2", 0))}U',
        f'#define BOARD_DESC_FLASH_LATENCY {int(clock.get("flash_latency", 0))}U',
        "",
    ]
    irq = analysis.get("irq", {})
    for name in ("uart", "dma", "exti", "systick"):
        lines.append(f"#define BOARD_DESC_IRQ_{name.upper()} {int(irq.get(name, 0))}U")
    lines.append("")

    for name, item in sorted(board.get("named_pins", {}).items()):
        macro = "BOARD_PIN_" + re.sub(r"[^A-Za-z0-9]", "_", name).upper()
        lines.append(f"#define {macro} {pin_number(item['pin'])}")
    lines.append("")

    for key in sorted(routes):
        route = routes[key]
        prefix = "BOARD_DMA_" + key
        lines.append(f"#define {prefix}_CONTROLLER {route['controller']}U")
        lines.append(f"#define {prefix}_STREAM {route['stream']}U")
        lines.append(f"#define {prefix}_CHANNEL {route['channel']}U")
    lines += ["", "#endif /* FW_GENERATED_BOARD_DESC_H */", ""]
    return "\n".join(lines)


def render_memory_ld(loaded: dict, symbols: dict) -> str:
    """生成 board_memory.ld；NVM 启用时从代码 Flash 尾部扣除。"""
    chip = loaded["chip"]
    board = loaded["board"]
    memory = chip["memory"]
    flash_origin = int(memory["flash"]["origin"], 0)
    flash_kb = int(memory["flash"]["size_kb"])
    ram_origin, ram_kb = _ram_extent(chip, [])
    ccm = memory.get("ccm", {})
    nvm = board.get("peripherals", {}).get("nvm")
    code_kb = flash_kb
    nvm_lines = []
    if nvm:
        nvm_origin = int(nvm["origin"], 0)
        nvm_kb = int(nvm["size_kb"])
        code_kb = flash_kb - nvm_kb
        nvm_lines = [
            f"BOARD_NVM_ORIGIN = {_hex32(nvm_origin)};",
            f"BOARD_NVM_LENGTH = {nvm_kb}K;",
        ]
    min_stack = _sym_int(symbols, "BOARD_MIN_STACK_SIZE", 4096)
    lines = [
        "/* Generated by tools/boardgen.py. Do not edit. */",
        "/* Board memory map; included by link.lds. */",
        "",
        f"BOARD_FLASH_ORIGIN = {_hex32(flash_origin)};",
        f"BOARD_FLASH_LENGTH = {flash_kb}K;",
        f"BOARD_CODE_FLASH_LENGTH = {code_kb}K;",
        *nvm_lines,
        f"BOARD_RAM_ORIGIN = {ram_origin};",
        f"BOARD_RAM_LENGTH = {ram_kb}K;",
    ]
    if ccm:
        lines += [
            f"BOARD_CCM_ORIGIN = {_hex32(int(ccm['origin'], 0))};",
            f"BOARD_CCM_LENGTH = {int(ccm['size_kb'])}K;",
        ]
    lines += [f"BOARD_MIN_STACK_SIZE = {min_stack};", ""]
    return "\n".join(lines)


def render_summary(loaded: dict, analysis: dict) -> str:
    """生成可人工审查的板级摘要。"""
    chip = loaded["chip"]
    board = loaded["board"]
    clock = analysis.get("clock", {})
    used = analysis.get("used", {})
    routes = analysis.get("routes", {})
    lines = [
        f"# board summary: {board.get('id')}",
        f"chip: {chip.get('id')}",
        f"clock variant: {clock.get('variant')}",
        f"sysclk: {clock.get('sysclk')} Hz",
        f"hclk: {clock.get('hclk')} Hz",
        f"pclk1: {clock.get('pclk1')} Hz",
        f"pclk2: {clock.get('pclk2')} Hz",
        f"manifest hash: {manifest_hash(loaded)}",
        "",
        "pins:",
    ]
    for (port, pin), label in sorted(used.items()):
        lines.append(f"  P{_pin_letter(port)}{pin:<2} {label}")
    lines += ["", "dma:"]
    if routes:
        for key in sorted(routes):
            route = routes[key]
            lines.append(f"  {key:<12} DMA{route['controller']} Stream{route['stream']} "
                         f"Channel{route['channel']} owner={route['owner']}")
    else:
        lines.append("  none")
    lines.append("")
    return "\n".join(lines)


def render_keil_info(loaded: dict, analysis: dict) -> dict:
    """生成 gen_keil.py 消费的器件/内存/时钟信息。"""
    chip = loaded["chip"]
    board = loaded["board"]
    memory = chip["memory"]
    hal = chip.get("hal", {})
    clock = analysis.get("clock", {})
    flash_origin = int(memory["flash"]["origin"], 0)
    flash_size = int(memory["flash"]["size_kb"]) * 1024
    ram_origin_label, ram_kb = _ram_extent(chip, [])
    ram_origin = int(ram_origin_label, 0)
    ram_size = ram_kb * 1024
    device = hal.get("keil_device", "")
    algo = hal.get("keil_flash_algo", "")
    cpu = hal.get("keil_cpu_template", "")
    cpu = (cpu.replace("@RAM_ORIGIN@", _hex32(ram_origin))
              .replace("@RAM_SIZE@", _hex32(ram_size))
              .replace("@FLASH_ORIGIN@", _hex32(flash_origin))
              .replace("@FLASH_SIZE@", _hex32(flash_size))
              .replace("@SYSCLK@", str(int(clock.get("sysclk", 0)))))
    flash_dll = (
        f"UL2CM3(-S0 -C0 -P0 -FD{ram_origin:08X} -FC1000 -FN1 -FF0{algo} "
        f"-FS0{flash_origin:X} -FL0{flash_size:X} "
        f"-FP0($$Device:{device}$Flash\\{algo}.FLM))"
    )
    register_file = str(hal.get("register_file", "")).replace("@DEVICE@", device)
    svd_file = str(hal.get("svd_file", "")).replace("@DEVICE@", device)
    return {
        "schema_version": 1,
        "board_id": board.get("id"),
        "chip_id": chip.get("id"),
        "vendor": hal.get("keil_vendor", ""),
        "device": device,
        "cpu": cpu,
        "flash_dll": flash_dll,
        "flash_origin": _hex32(flash_origin),
        "flash_size": _hex32(flash_size),
        "flash_origin_bare": f"{flash_origin:X}",
        "flash_size_bare": f"{flash_size:X}",
        "ram_origin": _hex32(ram_origin),
        "ram_size": _hex32(ram_size),
        "ram_size_bare": f"{ram_size:X}",
        "sysclk_hz": int(clock.get("sysclk", 0)),
        "flash_algorithm": algo,
        "register_file": register_file,
        "svd_file": svd_file,
        "manifest_hash": manifest_hash(loaded),
    }


def generate_extra(loaded: dict, symbols: dict, analysis: dict) -> dict[str, str]:
    """返回 genconfig 流水线需要的额外生成文件。"""
    if loaded.get("platform") == "host":
        return {}
    return {
        "board_desc.h": render_board_desc(loaded, analysis),
        "board_summary.txt": render_summary(loaded, analysis),
        "board_keil.json": json.dumps(render_keil_info(loaded, analysis),
                                      indent=2, sort_keys=True) + "\n",
    }
