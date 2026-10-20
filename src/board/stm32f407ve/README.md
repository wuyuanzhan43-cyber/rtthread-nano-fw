# Board: STM32F407VE

## 默认接线（沿用已验证配置）

| 功能 | 信号 | 引脚 | 说明 |
|---|---|---|---|
| LED | — | PE2 | 高电平点亮，`BOARD_LED_ACTIVE_HIGH=1` |
| console/protocol | USART1 TX | PA9 | AF7，115200 8N1，PA9 → USB-UART RX |
| console/protocol | USART1 RX | PA10 | AF7，PA10 ← USB-UART TX，共地 |
| LCD | SCL | PA5 | SPI1 AF5 |
| LCD | SDA/MOSI | PA7 | SPI1 AF5 |
| LCD | CS | PA4 | 低有效 |
| LCD | DC | PA6 | 命令/数据 |
| LCD | RES | PA3 | 硬件复位 |
| LCD | BL | PA2 | 背光，高有效 |

LCD 为 ST7735S 128×160，X/Y offset=2/1，MADCTL=0xC0，默认不反相。

## 时钟

- 默认：HSI 16 MHz，不启用 PLL，无需外部晶振。
- 可选：HSE 8 MHz → PLL → 168 MHz（要求板上确有 8 MHz 晶振）。
- 切换方式：`menuconfig` → `STM32F407 chip` → `System clock`。

## 设备与初始化顺序

```text
rt_hw_board_init (调度器启动前)
  HAL_Init
  NVIC 分组 4
  时钟树
  SysTick（HAL tick + rt_tick 共用）
  board_devices_init:
    drv_pin_register()        -> "pin"（单设备，覆盖全部 GPIO）
    drv_serial_register_all() -> "serial1..N"
    drv_spi_register_all()    -> "spi1..N"
    LCD 引脚配置 + st7735s_init -> "lcd"

main 线程
  fw_dev_open(console/pin)
  board_console_start()    arm 所有启用串口的 RX/NVIC
  board_boot_display()     LCD 初始化 + 头像 + 背光
  fw_demo_init + 1s 上报
  poll + fw_serial_wait_rx
```

LED 使用 `FW_PIN_NUMBER(BOARD_LED_PORT, BOARD_LED_PIN)`；默认 PE2 = 66。
LCD 的 CS/DC/RES/BL 也是 pin 编号，不再创建独立 `drv_pin_t` 对象。

## 中断优先级

- SysTick 与 PendSV：最低（`0xFF`）。
- USART：`BOARD_UART_IRQ_PRIORITY`，默认 6。
- RT-Thread 临界区使用 PRIMASK；serial class 通过 driver 的 `lock/unlock` 保护 ring。
- USART ISR 在 `drv_serial.c` 内定义，只碰 ring/事件，不阻塞。

## 调试

链接后可用以下命令确认强符号归属：

```text
arm-none-eabi-nm -n build/f407-debug/cmake/fw_v2.elf |
  grep -E "SysTick_Handler|rt_hw_console_output|PendSV_Handler|entry"
```

`SysTick_Handler`/`rt_hw_console_output` 应来自 board/driver，`PendSV_Handler`
来自 libcpu，`entry` 来自 RT-Thread components.c。
