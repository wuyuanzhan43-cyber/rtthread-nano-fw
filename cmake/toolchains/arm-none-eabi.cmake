# ===========================================================================
# ARM GNU 工具链（arm-none-eabi-gcc）
#
# 用于 STM32F407VE 固件：
#   CPU     Cortex-M4F
#   Thumb   指令集
#   FPU     fpv4-sp-d16（单精度硬件浮点）
#   ABI     hard-float
#
# 关键点：
#   - CMAKE_SYSTEM_NAME=Generic：告诉 CMake 这是裸机目标，
#     不尝试链接主机 libc/启动文件；
#   - TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY：编译器探测阶段只编译静态库，
#     避免因为缺少 _start/main 而误判工具链不可用；
#   - -ffunction-sections/-fdata-sections 配合链接器 --gc-sections 回收未用代码；
#   - 汇编文件使用 -x assembler-with-cpp，允许 #include 和预处理宏。
# ===========================================================================
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# 工具链必须存在于 PATH；找不到时 CMake 会立即报错。
find_program(CMAKE_C_COMPILER arm-none-eabi-gcc REQUIRED)
# 启动文件是 .S，用同一个 gcc 驱动做预处理和汇编。
set(CMAKE_ASM_COMPILER "${CMAKE_C_COMPILER}")
find_program(CMAKE_OBJCOPY arm-none-eabi-objcopy REQUIRED)
find_program(CMAKE_SIZE arm-none-eabi-size REQUIRED)

# 编译/链接共用的 CPU/FPU 参数；链接选项在 cmake/CMakeLists 中再次显式给出。
set(FW_CPU_FLAGS "-mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard")
set(CMAKE_C_FLAGS_INIT "${FW_CPU_FLAGS} -ffunction-sections -fdata-sections")
set(CMAKE_ASM_FLAGS_INIT "${FW_CPU_FLAGS} -x assembler-with-cpp")
