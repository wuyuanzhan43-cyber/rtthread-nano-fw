# ===========================================================================
# Embedded Framework v2 顶层 CMake
#
# 负责：
#   1. 选择 profile（host / f407ve / f407ve-hse）；
#   2. 调用 fwconfig.py sync 生成配置头文件；
#   3. 从 project/sources.json 读取唯一源文件清单；
#   4. 按平台创建 Host 测试可执行文件或 STM32 固件目标。
#
# 设计原则：
#   - 配置只来自 profiles/，不读 build/ 或 keil/ 下的配置副本；
#   - 源文件列表只来自 manifest，CMakeLists 不硬编码项目 .c；
#   - 生成物变化会触发 CMake 重新配置（CMAKE_CONFIGURE_DEPENDS）。
# ===========================================================================
cmake_minimum_required(VERSION 3.21)
project(fw_v2 VERSION 0.2.0 LANGUAGES C ASM)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- profile 选择 ----------------------------------------------------------
# FW_PROFILE 由 CMakePresets 设置：
#   host        - PC 单元测试；
#   f407ve      - HSI 16MHz 固件；
#   f407ve-hse  - HSE 168MHz 固件。
set(FW_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(FW_PROFILE "f407ve" CACHE STRING "Configuration profile under profiles/")
set(FW_PROFILE_DIR "${FW_ROOT}/profiles/${FW_PROFILE}")
set(FW_GEN_DIR "${FW_PROFILE_DIR}/generated")
set(FW_CONFIG_FILE "${FW_PROFILE_DIR}/.config")
set(FW_PROFILE_DEFCONFIG "${FW_PROFILE_DIR}/defconfig")

find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)

# --- 生成配置头文件 --------------------------------------------------------
# 每次 configure 都调用 sync：如果 .config 不存在会从 defconfig 初始化；
# 否则只重新生成内容有变化的文件。失败则终止 configure。
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${FW_ROOT}/tools/fwconfig.py"
            sync --profile "${FW_PROFILE}"
    RESULT_VARIABLE FW_GEN_RESULT)
if(NOT FW_GEN_RESULT EQUAL 0)
    message(FATAL_ERROR "Kconfig generation failed (profile ${FW_PROFILE})")
endif()

include("${FW_GEN_DIR}/fw_config.cmake")
include_directories(BEFORE "${FW_GEN_DIR}")

# 让 Ninja/CMake 在这些文件变化时自动重新 configure。
# 包括 profile、生成器本身和所有 Kconfig 源文件。
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${FW_CONFIG_FILE}"
    "${FW_PROFILE_DEFCONFIG}"
    "${FW_ROOT}/tools/fwconfig.py"
    "${FW_ROOT}/tools/genconfig.py"
    "${FW_ROOT}/tools/boardgen.py"
    "${FW_ROOT}/project/sources.json"
    "${FW_ROOT}/chips"
    "${FW_ROOT}/boards"
    "${FW_ROOT}/profiles/${FW_PROFILE}/profile.json"
    "${FW_ROOT}/Kconfig"
    "${FW_ROOT}/Kconfig.kernel"
    "${FW_ROOT}/Kconfig.framework"
    "${FW_ROOT}/Kconfig.app"
    "${FW_ROOT}/src/platform/stm32f407/Kconfig.drivers"
    "${FW_ROOT}/src/platform/board/stm32f407ve/Kconfig.board")

# 便捷目标：cmake --build ... --target menuconfig
# 直接编辑 profiles/<profile>/.config，退出后自动 sync。
add_custom_target(menuconfig
    COMMAND "${Python3_EXECUTABLE}" "${FW_ROOT}/tools/fwconfig.py"
            menuconfig --profile "${FW_PROFILE}"
    COMMENT "Edit profiles/${FW_PROFILE}/.config; the next build reconfigures"
    USES_TERMINAL)

# 统一警告选项：项目自有代码启用严格警告；第三方代码不使用本函数。
function(fw_warnings target)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE
            $<$<COMPILE_LANGUAGE:C>:-Wall;-Wextra;-Werror;-Wpedantic>)
    endif()
endfunction()

# --- 读取 manifest ---------------------------------------------------------
# sources.json 同时被 CMake 和 tools/gen_keil.py 使用。
# string(JSON) 是 CMake 3.19+ 内置功能，不需要额外 Python 解析。
file(READ "${FW_ROOT}/project/sources.json" FW_MANIFEST)

# 读取 JSON 数组并可选地加上 FW_ROOT 前缀，结果放到 output 变量。
function(fw_manifest_list key prefix output)
    string(JSON count LENGTH "${FW_MANIFEST}" ${key})
    set(items)
    if(count GREATER 0)
        math(EXPR last "${count} - 1")
        foreach(index RANGE ${last})
            string(JSON item GET "${FW_MANIFEST}" ${key} ${index})
            if(prefix)
                list(APPEND items "${FW_ROOT}/${item}")
            else()
                list(APPEND items "${item}")
            endif()
        endforeach()
    endif()
    set(${output} "${items}" PARENT_SCOPE)
endfunction()

# 项目私有包含目录/源文件（加绝对路径前缀）。
fw_manifest_list("includes" TRUE FW_INCLUDE_DIRS)
fw_manifest_list("target_includes" TRUE FW_TARGET_INCLUDE_DIRS)
# 宏定义不需要路径前缀。
fw_manifest_list("defines" FALSE FW_DEFINES)
# 各源文件分组。
fw_manifest_list("core_sources" TRUE FW_CORE_SOURCES)
fw_manifest_list("protocol_sources" TRUE FW_PROTOCOL_SOURCES)
fw_manifest_list("display_sources" TRUE FW_DISPLAY_SOURCES)
fw_manifest_list("task_sources" TRUE FW_TASK_SOURCES)
fw_manifest_list("app_sources" TRUE FW_APP_SOURCES)
fw_manifest_list("assets_sources" TRUE FW_ASSETS_SOURCES)
fw_manifest_list("rtport_sources" TRUE FW_RTPORT_SOURCES)
fw_manifest_list("main_sources" TRUE FW_MAIN_SOURCES)
fw_manifest_list("host_tests" TRUE FW_HOST_TESTS)
fw_manifest_list("bsp_sources" TRUE FW_BSP_SOURCES)
fw_manifest_list("board_sources" TRUE FW_BOARD_SOURCES)
fw_manifest_list("rtt_common_sources" TRUE FW_RTT_COMMON_SOURCES)
fw_manifest_list("rtt_gcc_sources" TRUE FW_RTT_GCC_SOURCES)
fw_manifest_list("rtt_ac5_sources" TRUE FW_RTT_AC5_SOURCES)
fw_manifest_list("hal_sources" TRUE FW_HAL_SOURCES)
fw_manifest_list("gcc_startup" TRUE FW_GCC_STARTUP)
fw_manifest_list("keil_startup" TRUE FW_KEIL_STARTUP)

# 所有目标共享项目包含目录（生成目录已经用 BEFORE 优先）。
include_directories(${FW_INCLUDE_DIRS})

# --- 平台无关库 ------------------------------------------------------------
# 这些库在 Host 和固件构建中都会被链接；Host 测试直接复用同一份逻辑。
add_library(fw_core STATIC ${FW_CORE_SOURCES})
fw_warnings(fw_core)

add_library(fw_protocol STATIC ${FW_PROTOCOL_SOURCES})
target_link_libraries(fw_protocol PUBLIC fw_core)
fw_warnings(fw_protocol)

add_library(fw_display STATIC ${FW_DISPLAY_SOURCES})
target_link_libraries(fw_display PUBLIC fw_core)
fw_warnings(fw_display)

add_library(fw_task STATIC ${FW_TASK_SOURCES})
target_link_libraries(fw_task PUBLIC fw_core)
fw_warnings(fw_task)

add_library(fw_assets STATIC ${FW_ASSETS_SOURCES})
fw_warnings(fw_assets)

add_library(fw_app STATIC ${FW_APP_SOURCES})
target_link_libraries(fw_app PUBLIC fw_protocol fw_task fw_display fw_assets)
fw_warnings(fw_app)

# --- 平台分支 --------------------------------------------------------------
if(CONFIG_FW_PLATFORM_HOST)
    # Host：编译测试可执行文件 + 6 个 CTest。
    add_executable(fw_v2_host_tests ${FW_HOST_TESTS})
    target_link_libraries(fw_v2_host_tests PRIVATE fw_app fw_task fw_display fw_core)
    fw_warnings(fw_v2_host_tests)
    enable_testing()
    add_test(NAME v2_host_tests COMMAND fw_v2_host_tests)

    find_package(Python3 3.8 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        # profile 生成物是否与 .config 一致。
        add_test(NAME v2_profiles_check
                 COMMAND "${Python3_EXECUTABLE}" "${FW_ROOT}/tools/fwconfig.py"
                         check --all)
        # 是否出现旧版配置路径或旧源码目录（configs/、旧 bsp/board 等）。
        add_test(NAME v2_config_layout_check
                 COMMAND "${Python3_EXECUTABLE}" "${FW_ROOT}/tools/fwconfig.py"
                         check-layout)
        # manifest 文件存在性/重复/覆盖性。
        add_test(NAME v2_manifest_check
                 COMMAND "${Python3_EXECUTABLE}" "${FW_ROOT}/tools/fwconfig.py"
                         check-manifest)
        # 第一方同名文件与旧命名残留。
        add_test(NAME v2_hygiene_check
                 COMMAND "${Python3_EXECUTABLE}" "${FW_ROOT}/tools/fwconfig.py"
                         check-hygiene)
        # chip/board manifest 与 Kconfig 的组合校验。
        add_test(NAME v2_board_check
                 COMMAND "${Python3_EXECUTABLE}" "${FW_ROOT}/tools/fwconfig.py"
                         check-boards)
        # 生成器/板级数据单元测试。
        add_test(NAME v2_tools_tests
                 COMMAND "${Python3_EXECUTABLE}" -m unittest discover
                         -s "${FW_ROOT}/tools/tests" -p "test_*.py")
        # Keil 工程是否与 manifest 同步。
        add_test(NAME v2_keil_project_check
                 COMMAND "${Python3_EXECUTABLE}" "${FW_ROOT}/tools/fwconfig.py"
                         gen-keil --profile f407ve --check)
        add_test(NAME v2_keil_project_hse_check
                 COMMAND "${Python3_EXECUTABLE}" "${FW_ROOT}/tools/fwconfig.py"
                         gen-keil --profile f407ve-hse --check)
    endif()
elseif(CONFIG_FW_PLATFORM_STM32F407VE)
    # 固件必须使用交叉工具链，防止误用主机 gcc 生成不可运行的 elf。
    if(NOT CMAKE_CROSSCOMPILING)
        message(FATAL_ERROR "STM32F407VE configuration requires cmake/toolchains/arm-none-eabi.cmake")
    endif()
    add_subdirectory(cmake)
else()
    message(FATAL_ERROR "No platform selected in ${FW_CONFIG_FILE}")
endif()
