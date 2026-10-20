# ===========================================================================
# STM32F407VE 固件构建
#
# 由顶层 CMakeLists 在 CONFIG_FW_PLATFORM_STM32F407VE 时 add_subdirectory 进入。
# 这里把 manifest 中的源文件组织成库：
#   rtt_kernel - RT-Thread Nano 内核（GCC 版本）
#   st_hal     - STM32F4 HAL/CMSIS 子集
#   fw_bsp     - pin/serial/spi 驱动（OBJECT 库，保留强符号覆盖）
#   fw_board   - 板级装配/时钟/console/IRQ
#   fw_rtport  - RT-Thread 与框架的适配层
#   fw_assets  - 启动图片
#   fw_v2      - 最终可执行文件
#
# 使用 OBJECT 库的原因：
#   board_console.c 需要提供强 rt_hw_console_output 覆盖内核弱符号；
#   OBJECT 库把目标文件直接交给最终链接，更接近 Keil 的“所有 .o 一起链接”。
# ===========================================================================
include("${FW_ROOT}/cmake/third_party.cmake")

# 板级目录：链接脚本 link.lds 在这里。
set(FW_BOARD_DIR "${FW_ROOT}/src/board/stm32f407ve")

# 芯片头文件/Nano 头文件对所有固件目标可见。
include_directories(${FW_TARGET_INCLUDE_DIRS})

add_library(rtt_kernel STATIC
    ${FW_RTT_COMMON_SOURCES}
    ${FW_RTT_GCC_SOURCES})
target_include_directories(rtt_kernel PUBLIC ${FW_TARGET_INCLUDE_DIRS})
# 内核是第三方代码：保留常见警告，但不使用 -Werror，避免上游风格触发失败。
target_compile_options(rtt_kernel PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-Wall;-Wextra;-Wno-unused-parameter;-Wno-sign-compare;-Wno-implicit-fallthrough>)

add_library(st_hal STATIC ${FW_HAL_SOURCES})
target_include_directories(st_hal PUBLIC ${FW_TARGET_INCLUDE_DIRS})
target_compile_definitions(st_hal PUBLIC ${FW_DEFINES})
# HAL 同样是第三方代码，只做适度警告检查。
target_compile_options(st_hal PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-Wall;-Wextra;-Wno-unused-parameter>)

# 自研 BSP：严格警告（fw_warnings），但允许变参宏（RT-Thread 风格）。
add_library(fw_bsp OBJECT ${FW_BSP_SOURCES})
target_link_libraries(fw_bsp PUBLIC rtt_kernel st_hal fw_core)
fw_warnings(fw_bsp)
target_compile_options(fw_bsp PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-Wno-variadic-macros>)

# 板级装配：依赖 bsp、display（LCD）和 core。
add_library(fw_board OBJECT ${FW_BOARD_SOURCES})
target_link_libraries(fw_board PUBLIC fw_bsp fw_display fw_core)
fw_warnings(fw_board)
target_compile_options(fw_board PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-Wno-variadic-macros>)

# RT-Thread 端口：目前只有毫秒时钟适配。
add_library(fw_rtport STATIC ${FW_RTPORT_SOURCES})
target_link_libraries(fw_rtport PUBLIC rtt_kernel fw_core)
fw_warnings(fw_rtport)
target_compile_options(fw_rtport PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-Wno-variadic-macros>)

# 启动图片：纯数据，没有依赖。
add_library(fw_assets STATIC ${FW_ASSETS_SOURCES})
fw_warnings(fw_assets)

# 最终固件：main + GCC 启动文件（startup_stm32f407xx.s）。
add_executable(fw_v2
    ${FW_MAIN_SOURCES}
    ${FW_GCC_STARTUP})

target_link_libraries(fw_v2 PRIVATE
    fw_board fw_bsp fw_rtport fw_app fw_display fw_assets fw_core rtt_kernel st_hal)
fw_warnings(fw_v2)
target_compile_options(fw_v2 PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-Wno-variadic-macros>)

# 链接参数：
#   -mcpu/-mthumb/-mfpu/-mfloat-abi 与编译选项一致（Cortex-M4F 硬浮点）；
#   -T link.lds 使用板级链接脚本（512KB Flash / 128KB SRAM）；
#   -e entry 指定 RT-Thread 的 entry 作为入口；
#   --gc-sections 回收未使用的函数/数据（配合 -ffunction-sections）；
#   nano.specs + nosys.specs 使用精简 libc 并忽略未实现系统调用；
#   -Map 输出符号/内存布局，便于分析 Flash/RAM 占用。
target_link_options(fw_v2 PRIVATE
    "-mcpu=cortex-m4" "-mthumb" "-mfloat-abi=hard" "-mfpu=fpv4-sp-d16"
    "-T${FW_BOARD_DIR}/link.lds"
    "-eentry"
    "-Wl,--gc-sections"
    "-Wl,--print-memory-usage"
    "-Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/fw_v2.map"
    "--specs=nano.specs"
    "--specs=nosys.specs")

set_target_properties(fw_v2 PROPERTIES SUFFIX ".elf")

# 构建后生成 hex/bin 并打印 size，方便直接烧录/检查。
add_custom_command(TARGET fw_v2 POST_BUILD
    COMMAND "${CMAKE_OBJCOPY}" -O ihex "$<TARGET_FILE_NAME:fw_v2>" fw_v2.hex
    COMMAND "${CMAKE_OBJCOPY}" -O binary "$<TARGET_FILE_NAME:fw_v2>" fw_v2.bin
    COMMAND "${CMAKE_SIZE}" "$<TARGET_FILE_NAME:fw_v2>"
    WORKING_DIRECTORY "$<TARGET_FILE_DIR:fw_v2>"
    VERBATIM)
