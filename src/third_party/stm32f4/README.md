# STM32F4 vendor subset

Self-contained subset used by this project:

- `hal/Inc` + selected `hal/Src` files: STM32F4xx HAL driver
- `device/Include`: `stm32f407xx.h`, `stm32f4xx.h`, `system_stm32f4xx.h`
- `device/Source/Templates/system_stm32f4xx.c`
- `cmsis/Include`: CMSIS Core headers
- `hal/Src` contains only the modules enabled in
  `src/bsp/st/stm32f407/stm32f4xx_hal_conf.h`.

Reason for the local copy: CMake derives object paths from the source path. When
the project path contains non-ASCII characters (this workspace contains
`学习`), putting vendor sources outside the project makes GCC 14's assembler fail
to create the `.obj` file on Windows. Keeping sources under the project makes
the object paths ASCII and the build reproducible.

Upstream licenses remain in the original files; see the ST/CMSIS headers for
terms. No external checkout is required at build time.
