# framework

| 目录 | 职责 | 依赖 |
|---|---|---|
| `include/fw/dev.h` | mr 风格设备核心：路径、ops、drv、open/close/read/write/ioctl/isr | 标准 C |
| `include/fw/{pin,serial,spi,display}.h` | 类型化设备类 | `fw/dev.h` |
| `core/dev.c` | 静态注册表、路径归一化、引用计数、ops 分发 | `fw/dev.h` |
| `core/{pin,serial,spi,display}.c` | 设备类包装；serial 有静态 ring + lock/unlock 注入 | `fw/dev.h` |
| `components/protocol/` | A5 5A 帧、CRC-16/CCITT-FALSE | `fw/status.h` |
| `components/display/` | ST7735S，使用 `spiN` + `pin` 设备 | `fw/display.h`、`fw/spi.h`、`fw/pin.h` |
| `app/` | PING/LED_SET/STATUS/STATS、0x90 上报、头像 | `fw/dev.h` + protocol |
| `ports/rtthread/` | RT-Thread tick → `fw_clock_t` | RT-Thread kernel |

## 红线

1. 不在这里包含 HAL/RT-Thread 头文件（`ports/` 除外）。
2. `core/dev.c` 不认识 pin/serial/spi/display 的具体类型；类型令牌由各 class 模块定义。
3. 业务只通过 `fw_dev_find/open/read/write/ioctl` 或 class helper 访问设备。
4. 所有内存静态分配；serial ring 由 driver/board 提供，不使用 `malloc`。

## 设备名

| 设备 | 语义 |
|---|---|
| `pin` | 一个设备覆盖所有 GPIO；offset = `(port << 4) | pin` |
| `serial1..N` | 每个启用的 UART 一个设备 |
| `spi1..N` | 每个启用的 SPI 一个设备 |
| `lcd` | ST7735S 显示设备 |

路径查找支持 `fw_dev_find("serial1")` 和 `fw_dev_find("/dev/serial1")`；当前是静态扁平注册表，不引入动态设备树。

## 扩展

新增设备类型（例如 ADC）：

```text
include/fw/adc.h     类型令牌、ops、fw_adc_t
core/adc.c           包装 + class 逻辑
src/bsp/st/driver/drv_adc.c
src/board/...        注册
project/sources.json 登记源文件
```

`core/dev.c` 不需要修改。

新增 UART/SPI 实例只改 Kconfig（`FW_USING_UARTn`/`FW_USING_SPIn` 和板级引脚），不需要修改任何 C/H。
