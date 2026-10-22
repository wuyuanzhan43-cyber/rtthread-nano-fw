# Board manifests

`boards/<board-id>/board.json` 描述板卡拓扑：

- 板卡身份、选用的芯片、时钟 variant；
- 命名引脚（LED/LCD 等）和设备绑定；
- UART/SPI/I2C/ADC/PWM 实例的实际接线与 AF；
- EXTI、DMA 策略和 NVM 分区（后续里程碑）。

`chips/<chip-id>.json` 描述芯片/封装能力：

- 端口、保留脚、内存区域；
- AF 矩阵、DMA 请求候选、EXTI 分组；
- 时钟上限、Flash latency 表、HAL/Keil 器件信息。

规则：

- manifest 是引脚/AF/时钟/内存/设备绑定的唯一事实源；
- Kconfig 只保留启用项、容量、ring/栈、IRQ 优先级、启动画面等构建选择；
- `profiles/<profile>/profile.json` 只选择 board/chip/clock variant，不复制事实；
- 修改 manifest 后运行 `python tools/fwconfig.py sync --all` 并重新生成 Keil 工程。
