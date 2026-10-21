/*
 * fw/task.h - OS 中立的任务注册、健康度与控制接口。
 *
 * 层级：src/framework/include/fw（实现位于 src/framework/components/task）
 * 职责：
 *   1. 用固定数组管理任务，不依赖 RTOS 类型；
 *   2. 通过 port ops 注入创建/启动/挂起/恢复/改优先级/快照能力；
 *   3. 提供栈高水位、CPU 占用、状态、心跳的健康度快照；
 *   4. 为 RPC 层提供摘要、详情和控制入口。
 *
 * 上下文：
 *   - create/adopt/start/control：任务上下文；
 *   - heartbeat/heartbeat_self：任务上下文；
 *   - refresh：监督任务上下文；
 *   - cpu_tick：SysTick 中断上下文；
 *   - summary/get_info：任意任务上下文，内部使用 port 临界区。
 *
 * 依赖：fw/status.h、fw/clock.h、fw_config.h、标准 C。
 *
 * 设计说明：
 *   - 所有任务和栈静态分配；本组件不调用 malloc；
 *   - 单实例：cpu_tick 使用模块内全局 manager 指针；
 *   - 框架不包含 rtthread.h，RT-Thread 适配在 port/rtthread。
 */
#ifndef FW_TASK_H
#define FW_TASK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "fw/status.h"
#include "fw/clock.h"
#include "fw_config.h"

/* 任务表容量；由 Kconfig 的 FW_TASK_MAX 覆盖。 */
#ifndef FW_TASK_MAX
#define FW_TASK_MAX 8
#endif

/* 任务名最大长度，含结尾 '\0'；与 RT_NAME_MAX 对齐，实际可见 7 字符。 */
#define FW_TASK_NAME_MAX 8

/* summary/detail 中“没有对应任务”的索引。 */
#define FW_TASK_NO_INDEX 0xFFU

/* 线程状态。RUNNING 由 port 在快照时根据当前线程补齐。 */
typedef enum {
    FW_TASK_STATE_UNKNOWN = 0,
    FW_TASK_STATE_INIT = 1,
    FW_TASK_STATE_READY = 2,
    FW_TASK_STATE_RUNNING = 3,
    FW_TASK_STATE_SUSPENDED = 4,
    FW_TASK_STATE_CLOSED = 5
} fw_task_state_t;

/* 健康度等级，数值越大越严重。 */
typedef enum {
    FW_TASK_HEALTH_OK = 0,
    FW_TASK_HEALTH_WARN = 1,
    FW_TASK_HEALTH_LOST = 2
} fw_task_health_t;

/* health 的具体原因位。 */
#define FW_TASK_REASON_STACK_LOW 0x01U
#define FW_TASK_REASON_HB_WARN   0x02U
#define FW_TASK_REASON_HB_LOST   0x04U
#define FW_TASK_REASON_CLOSED    0x08U

/* 运行时控制动作。 */
typedef enum {
    FW_TASK_CTRL_SUSPEND = 0,
    FW_TASK_CTRL_RESUME = 1,
    FW_TASK_CTRL_SET_PRIORITY = 2
} fw_task_ctrl_t;

typedef void (*fw_task_entry_t)(void *arg);

/*
 * 健康度策略。
 *
 * heartbeat_enabled - 是否参与心跳超时判定；
 * controllable      - 是否允许运行时 suspend/resume/set_priority；
 * heartbeat_warn_ms - 心跳年龄达到该值时标记 WARN；0 关闭；
 * heartbeat_lost_ms - 心跳年龄达到该值时标记 LOST；0 关闭；
 * stack_warn_free   - 栈剩余低于该字节数时标记 WARN；0 关闭。
 */
typedef struct {
    bool heartbeat_enabled;
    bool controllable;
    uint32_t heartbeat_warn_ms;
    uint32_t heartbeat_lost_ms;
    uint32_t stack_warn_free;
} fw_task_health_config_t;

/*
 * 创建新任务。
 *
 * stack/stack_size 由调用者提供静态存储；entry 是任务入口；
 * priority < port 返回的 max_priority；time_slice 为 RT-Thread 时间片。
 */
typedef struct {
    const char *name;
    fw_task_entry_t entry;
    void *arg;
    void *stack;
    size_t stack_size;
    uint8_t priority;
    uint16_t time_slice;
    fw_task_health_config_t health;
} fw_task_create_config_t;

/*
 * 接管一个已存在的线程。
 *
 * name 是 RT-Thread 线程名；不创建栈，也不启动线程。
 */
typedef struct {
    const char *name;
    fw_task_health_config_t health;
} fw_task_adopt_config_t;

/*
 * 单次 OS 快照。
 *
 * 由 port 填充；stack_used 是近似高水位，stack_size 是线程栈总大小。
 */
typedef struct {
    uint8_t state;
    uint8_t priority;
    uint32_t stack_size;
    uint32_t stack_used;
    uint32_t stack_free;
    bool closed;
} fw_task_os_info_t;

/*
 * 平台/OS 适配操作集。
 *
 * current_thread 返回当前线程的 os_task 句柄（可为 NULL）；
 * idle_thread 返回 idle 线程句柄；
 * critical_enter/exit 用于短小的计数器和快照保护。
 */
typedef struct {
    fw_status_t (*create)(void *ctx, const fw_task_create_config_t *config,
                          void **os_task);
    fw_status_t (*adopt)(void *ctx, const fw_task_adopt_config_t *config,
                         void **os_task);
    fw_status_t (*start)(void *ctx, void *os_task);
    fw_status_t (*suspend)(void *ctx, void *os_task);
    fw_status_t (*resume)(void *ctx, void *os_task);
    fw_status_t (*set_priority)(void *ctx, void *os_task, uint8_t priority);
    fw_status_t (*snapshot)(void *ctx, void *os_task, fw_task_os_info_t *info);
    void *(*current_thread)(void *ctx);
    void *(*idle_thread)(void *ctx);
    uint16_t (*max_priority)(void *ctx);
    uint32_t (*critical_enter)(void *ctx);
    void (*critical_exit)(void *ctx, uint32_t state);
    void *ctx;
} fw_task_port_ops_t;

struct fw_task_manager;

/* 单个托管任务的静态记录；字段由 fw_task_* API 维护。 */
typedef struct fw_task {
    struct fw_task_manager *manager;

    char name[FW_TASK_NAME_MAX];
    void *os_task;
    fw_task_entry_t entry;
    void *arg;
    void *stack;
    size_t stack_size;
    uint8_t priority;
    uint16_t time_slice;

    bool created;
    bool started;
    bool controllable;
    bool heartbeat_enabled;
    bool user_suspended;
    volatile bool suspend_pending;
    bool closed;

    uint32_t heartbeat_warn_ms;
    uint32_t heartbeat_lost_ms;
    uint32_t stack_warn_free;
    uint32_t last_heartbeat_ms;
    uint32_t heartbeat_count;

    volatile uint32_t cpu_ticks;
    uint32_t last_cpu_ticks;
    uint16_t cpu_permille;

    uint8_t state;
    uint8_t health;
    uint8_t reasons;
    uint32_t stack_used;
    uint32_t stack_free;
} fw_task_t;

/*
 * 任务管理器。
 *
 * cpu_*_ticks 在 SysTick 中断中自增；refresh 在临界区读取增量。
 */
typedef struct fw_task_manager {
    fw_task_t tasks[FW_TASK_MAX];
    size_t count;

    const fw_task_port_ops_t *port;
    fw_clock_t clock;
    uint32_t refresh_period_ms;
    uint32_t last_refresh_ms;
    uint16_t priority_max;

    volatile uint32_t cpu_total_ticks;
    volatile uint32_t cpu_idle_ticks;
    volatile uint32_t cpu_other_ticks;
    uint32_t last_total_ticks;
    uint32_t last_idle_ticks;
    uint32_t last_other_ticks;

    uint16_t busy_permille;
    uint16_t idle_permille;
    uint16_t other_permille;
    uint8_t worst_index;
    bool initialized;
} fw_task_manager_t;

typedef struct {
    const fw_task_port_ops_t *port;
    fw_clock_t clock;
    uint32_t refresh_period_ms;
} fw_task_manager_config_t;

typedef struct {
    uint8_t total;
    uint8_t ok;
    uint8_t warn;
    uint8_t lost;
    uint8_t worst_index;
    uint8_t current_index;
    uint16_t busy_permille;
    uint16_t idle_permille;
    uint16_t other_permille;
} fw_task_summary_t;

typedef struct {
    char name[FW_TASK_NAME_MAX];
    uint8_t index;
    uint8_t state;
    uint8_t health;
    uint8_t reasons;
    uint8_t priority;
    bool heartbeat_enabled;
    uint16_t cpu_permille;
    uint32_t stack_size;
    uint32_t stack_used;
    uint32_t stack_free;
    uint32_t heartbeat_age_ms;
    uint32_t heartbeat_count;
} fw_task_info_t;

/*
 * 初始化/反初始化管理器。
 *
 * 初始化会清空任务表并设置 cpu_tick 使用的全局 manager；
 * 反初始化只清空全局指针和标志，不销毁已创建的线程（测试用）。
 */
fw_status_t fw_task_manager_init(fw_task_manager_t *manager,
                                 const fw_task_manager_config_t *config);
void fw_task_manager_deinit(fw_task_manager_t *manager);

/*
 * 创建/接管任务。
 *
 * 失败返回 NULL；成功返回任务表中的稳定指针。
 * create 不会自动启动；需要再调用 fw_task_start。
 */
fw_task_t *fw_task_create(fw_task_manager_t *manager,
                          const fw_task_create_config_t *config);
fw_task_t *fw_task_adopt(fw_task_manager_t *manager,
                         const fw_task_adopt_config_t *config);

/* 启动已创建但未启动的任务。 */
fw_status_t fw_task_start(fw_task_manager_t *manager, fw_task_t *task);

/* 更新指定任务的心跳时间和计数。 */
void fw_task_heartbeat(fw_task_t *task);

/*
 * 更新当前任务的心跳；同时处理 suspend_pending：
 * 被请求挂起的任务会在这里自行 suspend + schedule。
 */
void fw_task_heartbeat_self(fw_task_manager_t *manager);

/* 刷新状态/栈/心跳/CPU 窗口；应由监督任务周期调用。 */
void fw_task_refresh(fw_task_manager_t *manager);

/* 距离下一次 refresh 或心跳阈值的毫秒数；到期返回 0。 */
uint32_t fw_task_next_wait_ms(const fw_task_manager_t *manager);

/* 摘要与按索引详情。 */
fw_status_t fw_task_summary(const fw_task_manager_t *manager,
                            fw_task_summary_t *summary);
fw_status_t fw_task_get_info(const fw_task_manager_t *manager, size_t index,
                             fw_task_info_t *info);

/*
 * 运行时控制。
 *
 * suspend 只置 pending，由目标任务在 heartbeat_self 中协作挂起；
 * resume 只接受 user_suspended 的任务；
 * set_priority 的值必须小于 port 的 max_priority。
 */
fw_status_t fw_task_control(fw_task_manager_t *manager, size_t index,
                            fw_task_ctrl_t action, uint8_t value);

/* SysTick 中断调用；未初始化时安全返回。 */
void fw_task_cpu_tick(void);

#endif
