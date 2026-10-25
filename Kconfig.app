# ---------------------------------------------------------------------------
# 应用层配置
#
# 这些选项属于业务/表现层，不属于 framework 或 board。
# 生成到 app_config.h（APP_ 前缀）。
# ---------------------------------------------------------------------------
menu "Application"

# 是否绘制 LCD 启动画面。关闭后 app_boot_display 直接返回 FW_ENOTSUP。
config APP_BOOT_DISPLAY
    bool "Draw LCD boot screen"
    depends on FW_USE_DISPLAY
    default y

# 启动画面三选一：
#   Color bars - 四色横条，用于检查 SPI/窗口/颜色顺序；
#   Avatar     - 128x160 图片，验证整屏位图传输（占 40KB Flash）；
#   White      - 纯白，检查背光和供电。
choice APP_BOOT_SCREEN
    prompt "Boot screen"
    depends on APP_BOOT_DISPLAY
    default APP_BOOT_IMAGE

config APP_BOOT_BARS
    bool "Color bars"

config APP_BOOT_IMAGE
    bool "Avatar image"

config APP_BOOT_WHITE
    bool "White diagnostic"
endchoice

# 隐藏的数值符号，供 app_boot.c 的 #if 使用。
# 最后一条 default 0 保证关闭 APP_BOOT_DISPLAY 时仍有合法值。
config APP_BOOT_MODE
    int
    default 0 if APP_BOOT_BARS
    default 1 if APP_BOOT_IMAGE
    default 2 if APP_BOOT_WHITE
    default 0


# ---------------------------------------------------------------------------
# Task manager / RPC thread
# ---------------------------------------------------------------------------
config APP_RPC_TASK_STACK_SIZE
    int "RPC task stack (bytes)"
    default 2048
    range 512 16384

config APP_RPC_TASK_PRIORITY
    int "RPC task priority"
    default 9
    range 0 31

# RPC 任务等待上限。必须明显小于心跳 warn 阈值，
# 否则任务长时间睡在串口等待里会被误判为心跳超时。
config APP_RPC_LOOP_WAIT_MAX_MS
    int "RPC task max wait (ms)"
    default 500
    range 10 5000

# 可控 worker 任务：用于验证任务控制（0x07 suspend/resume）和
# 多任务健康度/CPU 统计。它不是业务必需的；如果关闭，需要从
# app_task.c 中同时移除对应创建逻辑。
config APP_WORKER_TASK_STACK_SIZE
    int "Worker task stack (bytes)"
    default 1024
    range 512 8192

config APP_WORKER_TASK_PRIORITY
    int "Worker task priority"
    default 12
    range 0 31

config APP_WORKER_PERIOD_MS
    int "Worker loop period (ms)"
    default 100
    range 10 5000

config APP_HEALTH_REFRESH_MS
    int "Health refresh period (ms)"
    default 1000
    range 100 60000

config APP_TASK_HEARTBEAT_WARN_MS
    int "Heartbeat warn threshold (ms)"
    default 1500
    range 100 60000

config APP_TASK_HEARTBEAT_LOST_MS
    int "Heartbeat lost threshold (ms)"
    default 3000
    range 100 120000

config APP_TASK_STACK_WARN_BYTES
    int "Low stack free threshold (bytes)"
    default 256
    range 0 16384


endmenu
