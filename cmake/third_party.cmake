# ===========================================================================
# 第三方 STM32F4 子集
#
# 为什么把 HAL/CMSIS/device 放进项目内：
#   项目路径包含中文时，GCC 14 的汇编器在处理外部绝对路径的 .S 文件时
#   可能失败；把第三方源码放在项目内并使用相对源路径可以避免这个问题。
#   同时固定了版本，避免不同机器上的 Cube 包差异。
#
# 这里只做路径定义和存在性检查；实际源文件列表在 project/sources.json
# 的 hal_sources 中，由顶层 CMake 读取。
# ===========================================================================
set(FW_THIRD_PARTY_DIR "${FW_ROOT}/src/third_party/stm32f4"
    CACHE PATH "STM32F4 vendor subset")
set(HAL_DIR "${FW_THIRD_PARTY_DIR}/hal")
set(DEVICE_DIR "${FW_THIRD_PARTY_DIR}/device")
set(CMSIS_DIR "${FW_THIRD_PARTY_DIR}/cmsis")

# 早期失败：如果子模块/文件缺失，给出明确路径而不是链接期报错。
foreach(required
        "${HAL_DIR}/Inc/stm32f4xx_hal.h"
        "${DEVICE_DIR}/Include/stm32f407xx.h"
        "${DEVICE_DIR}/Source/Templates/system_stm32f4xx.c"
        "${CMSIS_DIR}/Include/core_cm4.h")
    if(NOT EXISTS "${required}")
        message(FATAL_ERROR "Missing ${required}")
    endif()
endforeach()
