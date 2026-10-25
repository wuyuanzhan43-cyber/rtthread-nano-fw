# STM32F4 vendor subset

Self-contained subset used by this project:

- `hal/Inc` + selected `hal/Src` files: STM32F4xx HAL driver V1.8.3
- `device/Include`: `stm32f407xx.h`, `stm32f4xx.h`, `system_stm32f4xx.h`
- `device/Source/Templates/system_stm32f4xx.c`
- `cmsis/Include`: CMSIS Core headers
- `hal/Src` contains only the modules enabled in
  `src/platform/stm32f407/stm32f4xx_hal_conf.h`.

Enabled HAL modules (sources present under `hal/Src`):

```text
stm32f4xx_hal.c            stm32f4xx_hal_dma.c
stm32f4xx_hal_dma_ex.c     stm32f4xx_hal_flash.c
stm32f4xx_hal_flash_ex.c   stm32f4xx_hal_gpio.c
stm32f4xx_hal_pwr.c        stm32f4xx_hal_pwr_ex.c
stm32f4xx_hal_rcc.c        stm32f4xx_hal_rcc_ex.c
stm32f4xx_hal_spi.c        stm32f4xx_hal_uart.c
stm32f4xx_hal_i2c.c        stm32f4xx_hal_i2c_ex.c
stm32f4xx_hal_adc.c        stm32f4xx_hal_adc_ex.c
stm32f4xx_hal_tim.c        stm32f4xx_hal_tim_ex.c
stm32f4xx_hal_iwdg.c
```

Reason for the local copy: CMake derives object paths from the source path. When
the project path contains non-ASCII characters (this workspace contains
`学习`), putting vendor sources outside the project makes GCC 14's assembler fail
to create the `.obj` file on Windows. Keeping sources under the project makes
the object paths ASCII and the build reproducible.

## Import provenance

- Upstream: STMicroelectronics STM32CubeF4, STM32F4xx HAL Driver **V1.8.3**.
- The I2C/ADC/TIM modules (`stm32f4xx_hal_i2c.c`, `stm32f4xx_hal_i2c_ex.c`,
  `stm32f4xx_hal_adc.c`, `stm32f4xx_hal_adc_ex.c`, `stm32f4xx_hal_tim.c`,
  `stm32f4xx_hal_tim_ex.c`, `stm32f4xx_hal_iwdg.c`) were copied from the local V1.8.3 HAL tree
  `46.嵌入式框架设计1/third_party/hal-src` on 2026-10-25. Their headers were
  verified byte-identical to the headers already in `hal/Inc`.
- License: BSD-3-Clause, see [`LICENSE.md`](LICENSE.md). Upstream license
  headers remain in each source file.
- No external checkout is required at build time.
