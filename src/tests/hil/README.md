# HIL 协议冒烟测试

目标：用真实串口验证本工程固件的接收方向、协议回复和噪声恢复。

## 准备

1. 固件已烧录，USART1 PA9/PA10 接到 USB-UART（PA9→适配器 RX，
   PA10←适配器 TX，共地）。
2. 安装依赖：

```text
python -m pip install -r tests/hil/requirements.txt
```

## 运行

在项目根目录：

```text
python tests/hil/test_protocol.py --port COM7
python tests/hil/test_protocol.py --port COM7 --stats
```

脚本验证：

1. 周期主动上报 `0x90`；
2. PING → `PONG`；
3. LED_SET 开/关 + STATUS 状态一致性；
4. 未知命令状态码 2；
5. 非法参数状态码 1；
6. 前导噪声后的帧恢复；
7. 分片帧；
8. `--stats` 时验证 STATS 计数器。

## 说明

- 线协议与旧版完全一致，脚本原样复用。
- 默认 UART IT 路径；`BOARD_UART_DMA=y` 尚未实现，保持 `n`。
- 启动 banner 是 ASCII 噪声，脚本按 `A5 5A` 搜索帧，不受影响。
