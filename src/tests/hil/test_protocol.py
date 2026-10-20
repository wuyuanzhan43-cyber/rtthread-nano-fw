#!/usr/bin/env python3
"""硬件在环（HIL）串口协议冒烟测试。

用法：
    python src/tests/hil/test_protocol.py --port COM7
    python src/tests/hil/test_protocol.py --port /dev/ttyUSB0 --baud 115200
    python src/tests/hil/test_protocol.py --port COM7 --stats

前提：
    - 安装 pyserial；
    - 板子运行 protocol 模式固件（APP_UART_HELLO_TEST=0）；
    - USB-UART 适配器的 TX/RX/GND 与板子正确交叉连接；
    - 波特率与固件 BOARD_UART_BAUD 一致（默认 115200）。

测试内容：
    1. 等待固件主动上报 0x90；
    2. PING -> PONG；
    3. LED 开/关 + STATUS；
    4. 未知命令；
    5. 非法 LED 参数；
    6. 噪声字节后重新同步；
    7. 分片发送一帧（验证逐字节状态机）；
    8. 可选 STATS 统计。

退出码：
    0 通过；1 协议断言失败；2 串口打开/读写错误。
"""
import argparse
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial is required: python -m pip install pyserial")

# 与固件 fw/protocol.h、demo_app.h 保持一致的常量。
MAX_PAYLOAD = 32
CMD_PING = 0x01
CMD_LED_SET = 0x02
CMD_STATUS = 0x03
CMD_STATS = 0x04
EVENT_STATUS = 0x90
REPLY = 0x80


def crc16(data):
    """CRC-16/CCITT-FALSE，与固件 fw_crc16 完全一致。

    init=0xFFFF、poly=0x1021、不反射、xorout=0。
    逐位实现便于对照 C 代码；HIL 数据量很小，不需要查表。
    """
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def encode(command, sequence, payload=b""):
    """把命令编码成完整帧：A5 5A LEN SEQ CMD PAYLOAD CRC_LO CRC_HI。

    多字节 CRC 小端发送；body（LEN/SEQ/CMD/payload）参与 CRC。
    """
    body = bytes([len(payload), sequence, command]) + payload
    crc = crc16(body)
    return b"\xA5\x5A" + body + bytes([crc & 0xFF, crc >> 8])


class FrameReader:
    """从串口字节流中提取合法帧。

    维护一个 bytearray 缓冲区：
      - 每次 read_frame 先尝试从已有数据中取帧；
      - 不够一帧就继续 read(1)；
      - 超过 deadline 返回 None。

    取帧逻辑与固件 parser 类似，但使用 Python 的 bytes.find 简化：
      - 找 A5 5A；
      - 检查 LEN <= 32；
      - 等够 LEN+7 字节；
      - 校验 CRC；
      - 返回 (sequence, command, payload)。
    """

    def __init__(self, port):
        self.port = port
        self.buffer = bytearray()

    def read_frame(self, timeout):
        """在 timeout 秒内读取并返回一帧，超时返回 None。"""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            frame = self._take_frame()
            if frame is not None:
                return frame
            chunk = self.port.read(1)
            if chunk:
                self.buffer.extend(chunk)
        return None

    def _take_frame(self):
        """尝试从缓冲区头部取出一帧；数据不足时返回 None。

        关键点：
          - 丢弃前导码之前的噪声；
          - LEN 非法时只删一个字节，继续寻找下一个 A5 5A；
          - CRC 错误时整帧丢弃并继续循环；
          - 返回 payload 的切片，不包含 LEN/SEQ/CMD/CRC。
        """
        while True:
            index = self.buffer.find(b"\xA5\x5A")
            if index < 0:
                # 保留最后一个字节，防止 A5 被拆在两次读取之间。
                if len(self.buffer) > 1:
                    del self.buffer[:-1]
                return None
            if index:
                del self.buffer[:index]
            if len(self.buffer) < 3:
                return None

            length = self.buffer[2]
            if length > MAX_PAYLOAD:
                # 非法长度：跳过当前 A5，继续寻找下一对前导码。
                del self.buffer[:1]
                continue

            total = length + 7
            if len(self.buffer) < total:
                return None

            frame = bytes(self.buffer[:total])
            del self.buffer[:total]

            body = frame[2:total - 2]
            received = frame[total - 2] | (frame[total - 1] << 8)
            if crc16(body) != received:
                continue  # 坏帧：丢弃后继续处理缓冲区中的后续数据

            # body = LEN, SEQ, CMD, payload...
            return frame[3], frame[4], frame[5:5 + length]


def wait_reply(reader, command, sequence, timeout=2.0):
    """等待指定命令/序号的回复，忽略主动上报。

    固件会周期性发送 0x90，因此不能简单读下一帧；
    这里循环读取，直到命令匹配 command|0x80 且序号一致。
    超时抛出 AssertionError。
    """
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        frame = reader.read_frame(deadline - time.monotonic())
        if frame is None:
            break
        seq, cmd, payload = frame
        if cmd == EVENT_STATUS:
            continue  # 主动上报不是回复
        if cmd == (command | REPLY) and seq == sequence:
            return payload
    raise AssertionError("no reply for cmd 0x%02X seq %u" % (command, sequence))


def expect(condition, message):
    """轻量断言：失败时抛 AssertionError，由 main 统一捕获并打印。"""
    if not condition:
        raise AssertionError(message)


def main():
    """执行完整的 HIL 冒烟流程。"""
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True)
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=5.0)
    parser.add_argument("--stats", action="store_true",
                        help="also test CMD_STATS (requires current firmware)")
    args = parser.parse_args()

    # timeout=0.1 让 read(1) 不会无限阻塞；整体超时由 read_frame 控制。
    with serial.Serial(args.port, args.baud, timeout=0.1) as port:
        reader = FrameReader(port)
        port.reset_input_buffer()

        # 1. 固件每 1 秒主动上报；收到说明串口和协议基本正常。
        print("1. waiting for autonomous 0x90 report...")
        frame = reader.read_frame(args.timeout)
        expect(frame is not None, "no autonomous report; check wiring/baud/mode")
        seq, cmd, payload = frame
        expect(cmd == EVENT_STATUS, "expected 0x90, got 0x%02X" % cmd)
        expect(len(payload) == 3 and payload[0] == 0, "bad report payload")
        print("   ok (seq=%u known=%u state=%u)" % (seq, payload[1], payload[2]))

        # 2. PING：空 payload，回复状态 0 + "PONG"。
        print("2. PING")
        port.write(encode(CMD_PING, 1))
        payload = wait_reply(reader, CMD_PING, 1)
        expect(payload[:1] == b"\x00" and payload[1:] == b"PONG", "bad PING reply")

        # 3. LED 开 -> STATUS 应为 known=1, state=1；关 -> state=0。
        print("3. LED on, STATUS, LED off, STATUS")
        port.write(encode(CMD_LED_SET, 2, b"\x01"))
        payload = wait_reply(reader, CMD_LED_SET, 2)
        expect(payload[0] == 0, "LED on rejected")
        port.write(encode(CMD_STATUS, 3))
        payload = wait_reply(reader, CMD_STATUS, 3)
        expect(payload == b"\x00\x01\x01", "STATUS after LED on: %r" % payload)
        port.write(encode(CMD_LED_SET, 4, b"\x00"))
        payload = wait_reply(reader, CMD_LED_SET, 4)
        expect(payload[0] == 0, "LED off rejected")
        port.write(encode(CMD_STATUS, 5))
        payload = wait_reply(reader, CMD_STATUS, 5)
        expect(payload == b"\x00\x01\x00", "STATUS after LED off: %r" % payload)

        # 4. 未知命令：固件回复状态 2。
        print("4. unknown command")
        port.write(encode(0x7F, 6))
        payload = wait_reply(reader, 0x7F, 6)
        expect(payload[0] == 2, "unknown command status: %r" % payload)

        # 5. LED 参数非法（2）：固件回复状态 1，不改变硬件。
        print("5. bad LED payload")
        port.write(encode(CMD_LED_SET, 7, b"\x02"))
        payload = wait_reply(reader, CMD_LED_SET, 7)
        expect(payload[0] == 1, "bad argument status: %r" % payload)

        # 6. 先发噪声，再发合法帧；parser 应能在 A5 5A 处重新同步。
        print("6. noise recovery")
        port.write(b"\x00\xA5\x00")
        port.write(encode(CMD_PING, 8))
        payload = wait_reply(reader, CMD_PING, 8)
        expect(payload[:1] == b"\x00" and payload[1:] == b"PONG", "noise recovery failed")

        # 7. 把一帧拆成两次写，验证逐字节状态机能跨读边界组帧。
        print("7. fragmented frame")
        frame = encode(CMD_PING, 9)
        port.write(frame[:3])
        time.sleep(0.05)
        port.write(frame[3:])
        payload = wait_reply(reader, CMD_PING, 9)
        expect(payload[:1] == b"\x00" and payload[1:] == b"PONG", "fragmented frame failed")

        # 8. 可选统计：除了字段长度，还检查帧数至少为 9（前面已发多帧）。
        if args.stats:
            print("8. STATS")
            port.write(encode(CMD_STATS, 10))
            payload = wait_reply(reader, CMD_STATS, 10)
            expect(len(payload) == 14 and payload[0] == 0,
                   "bad STATS reply: %r" % payload)
            frames = int.from_bytes(payload[1:5], "little")
            io_errors = int.from_bytes(payload[5:9], "little")
            partials = int.from_bytes(payload[9:13], "little")
            expect(frames >= 9, "STATS frames too small: %u" % frames)
            print("   ok (frames=%u io_errors=%u partial_timeouts=%u report_seq=%u)"
                  % (frames, io_errors, partials, payload[13]))

    print("PASS HIL protocol smoke test")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as error:
        # 协议/内容不符合预期：退出码 1。
        print("FAIL:", error)
        sys.exit(1)
    except serial.SerialException as error:
        # 串口打开失败/掉线：退出码 2，便于脚本区分环境问题。
        print("SERIAL ERROR:", error)
        sys.exit(2)
