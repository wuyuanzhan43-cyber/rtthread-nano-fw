/*
 * stm32f407_fault.c - Cortex-M4 故障入口与现场输出。
 *
 * 层级：src/platform/stm32f407
 * 职责：
 *   1. 覆盖启动文件中的 weak HardFault/MemManage/BusFault/UsageFault；
 *   2. 从 MSP/PSP 取出异常栈帧，保存 SCB 故障寄存器；
 *   3. 绕过 serial ring，直接轮询控制台输出；
 *   4. 按 BOARD_FAULT_RESET 选择复位或停机。
 *
 * 上下文：异常上下文。这里不能调用 RT-Thread、HAL 延时或任何阻塞 API；
 * 只使用寄存器访问和一个很小的行缓冲。
 *
 * 依赖：stm32f407_fault.h、stm32f407_console.h、board_config.h。
 */
#include "stm32f407_fault.h"
#include "stm32f407_console.h"
#include "board_config.h"
#include "stm32f4xx_hal.h"
#include <rthw.h>
#include <rtthread.h>

volatile uint32_t g_stm32f407_fault_frame[8];
volatile uint32_t g_stm32f407_fault_cfsr;
volatile uint32_t g_stm32f407_fault_hfsr;
volatile uint32_t g_stm32f407_fault_mmfar;
volatile uint32_t g_stm32f407_fault_bfar;
volatile uint32_t g_stm32f407_fault_exc_return;
volatile uint32_t g_stm32f407_fault_vector;

/* 96 字节行缓冲；故障路径不做动态分配。 */
static char s_line[96];
static unsigned s_len;

static void line_reset(void)
{
    s_len = 0U;
}

static void line_char(char c)
{
    if (s_len < sizeof(s_line) - 1U) s_line[s_len++] = c;
}

static void line_str(const char *text)
{
    while (text != NULL && *text != '\0') line_char(*text++);
}

static void line_hex(uint32_t value)
{
    static const char digits[] = "0123456789ABCDEF";
    int shift;

    line_str("0x");
    for (shift = 28; shift >= 0; shift -= 4)
        line_char(digits[(value >> shift) & 0xFU]);
}

static void line_flush(void)
{
    s_line[s_len] = '\0';
    stm32f407_console_panic_write(s_line);
    s_len = 0U;
}

/* 根据 ICSR.VECTACTIVE 判断故障类型。 */
static const char *fault_name(uint32_t vector)
{
    switch (vector) {
    case 3U: return "HardFault";
    case 4U: return "MemManage";
    case 5U: return "BusFault";
    case 6U: return "UsageFault";
    default: return "Fault";
    }
}

/*
 * 公共 C 处理体。
 *
 * 帧顺序遵循 ARMv7-M 异常压栈：
 *   R0 R1 R2 R3 R12 LR PC xPSR。
 */
#if defined(__CC_ARM)
void stm32f407_fault_dump(uint32_t *frame, uint32_t exc_return)
#else
__attribute__((used, noinline))
void stm32f407_fault_dump(uint32_t *frame, uint32_t exc_return)
#endif
{
    uint32_t vector = SCB->ICSR & SCB_ICSR_VECTACTIVE_Msk;
    unsigned i;

    g_stm32f407_fault_cfsr = SCB->CFSR;
    g_stm32f407_fault_hfsr = SCB->HFSR;
    g_stm32f407_fault_mmfar = SCB->MMFAR;
    g_stm32f407_fault_bfar = SCB->BFAR;
    g_stm32f407_fault_exc_return = exc_return;
    g_stm32f407_fault_vector = vector;
    for (i = 0U; i < 8U; ++i) {
        g_stm32f407_fault_frame[i] = (frame != NULL) ? frame[i] : 0U;
    }

    line_reset();
    line_str("*** FAULT: ");
    line_str(fault_name(vector));
    line_flush();

    line_reset();
    line_str("  CFSR=");
    line_hex(g_stm32f407_fault_cfsr);
    line_str(" HFSR=");
    line_hex(g_stm32f407_fault_hfsr);
    line_flush();

    line_reset();
    line_str("  MMFAR=");
    line_hex(g_stm32f407_fault_mmfar);
    line_str(" BFAR=");
    line_hex(g_stm32f407_fault_bfar);
    line_flush();

    line_reset();
    line_str("  R0=");
    line_hex(g_stm32f407_fault_frame[0]);
    line_str(" R1=");
    line_hex(g_stm32f407_fault_frame[1]);
    line_str(" R2=");
    line_hex(g_stm32f407_fault_frame[2]);
    line_str(" R3=");
    line_hex(g_stm32f407_fault_frame[3]);
    line_flush();

    line_reset();
    line_str("  R12=");
    line_hex(g_stm32f407_fault_frame[4]);
    line_str(" LR=");
    line_hex(g_stm32f407_fault_frame[5]);
    line_str(" PC=");
    line_hex(g_stm32f407_fault_frame[6]);
    line_flush();

    line_reset();
    line_str("  xPSR=");
    line_hex(g_stm32f407_fault_frame[7]);
    line_str(" EXC_RETURN=");
    line_hex(exc_return);
    line_flush();

#if defined(BOARD_FAULT_RESET) && BOARD_FAULT_RESET
    line_reset();
    line_str("*** resetting\n");
    line_flush();
    NVIC_SystemReset();
#endif

    line_reset();
    line_str("*** halted\n");
    line_flush();
    for (;;) {
        __NOP();
    }
}

/*
 * RT-Thread 的 HardFault 入口会先调用异常钩子；钩子拿到的是
 * exception_stack_frame（R0-R3/R12/LR/PC/xPSR），格式与本模块一致。
 * 钩子不返回，直接按 BOARD_FAULT_RESET 停机或复位。
 */
static rt_err_t fault_exception_hook(void *context)
{
    stm32f407_fault_dump((uint32_t *)context, 0U);
    return -RT_ERROR;
}

void stm32f407_fault_init(void)
{
    /* 打开独立 fault 通道，避免全部升级成 HardFault。 */
    SCB->SHCSR |= SCB_SHCSR_MEMFAULTENA_Msk |
                  SCB_SHCSR_BUSFAULTENA_Msk |
                  SCB_SHCSR_USGFAULTENA_Msk;
    rt_hw_exception_install(fault_exception_hook);
}

/*
 * MemManage/BusFault/UsageFault 仍由本模块的强符号入口处理；
 * 入口选择当前异常使用的是 MSP 还是 PSP，把帧指针放进 R0、
 * EXC_RETURN 放进 R1，然后跳转公共 C 处理体。
 */
#if defined(__CC_ARM)
__asm void MemManage_Handler(void)
{
    IMPORT stm32f407_fault_dump
    TST LR, #4
    ITE EQ
    MRSEQ R0, MSP
    MRSNE R0, PSP
    MOV R1, LR
    B stm32f407_fault_dump
}

__asm void BusFault_Handler(void)
{
    IMPORT stm32f407_fault_dump
    TST LR, #4
    ITE EQ
    MRSEQ R0, MSP
    MRSNE R0, PSP
    MOV R1, LR
    B stm32f407_fault_dump
}

__asm void UsageFault_Handler(void)
{
    IMPORT stm32f407_fault_dump
    TST LR, #4
    ITE EQ
    MRSEQ R0, MSP
    MRSNE R0, PSP
    MOV R1, LR
    B stm32f407_fault_dump
}
#else
#define FAULT_HANDLER(name)                                                   \
    __attribute__((naked)) void name(void)                                    \
    {                                                                         \
        __asm volatile("tst lr, #4\n"                                         \
                       "ite eq\n"                                             \
                       "mrseq r0, msp\n"                                      \
                       "mrsne r0, psp\n"                                      \
                       "mov r1, lr\n"                                         \
                       "b stm32f407_fault_dump\n");                           \
    }

FAULT_HANDLER(MemManage_Handler)
FAULT_HANDLER(BusFault_Handler)
FAULT_HANDLER(UsageFault_Handler)

#undef FAULT_HANDLER
#endif
