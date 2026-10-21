/*
 * v2 STM32F407VE 链接脚本（GNU ld）
 *
 * 存储器：
 *   FLASH 512KB @ 0x08000000 - 中断向量、代码、只读数据、.data 初值；
 *   RAM   128KB @ 0x20000000 - .data、.bss、主栈/中断栈、RT-Thread 静态对象；
 *   CCM    64KB @ 0x10000000 - 故意不使用：DMA 无法访问 CCM，
 *                              为了保持所有缓冲区一致可 DMA，统一放主 SRAM。
 *
 * 符号约定（启动文件/RT-Thread 会引用）：
 *   _estack       - 栈顶（RAM 末尾），复位时装入 MSP；
 *   _sidata       - .data 在 Flash 中的加载地址；
 *   _sdata/_edata - .data 在 RAM 中的起止；
 *   _sbss/_ebss   - .bss 起止；
 *   end/_end      - 堆起点（RT_USING_HEAP 时由 platform boot 使用）。
 */
ENTRY(Reset_Handler)

/* 区域大小/起始地址来自 Kconfig.board，由 genconfig.py 生成。 */
INCLUDE board_memory.ld

MEMORY {
    FLASH (rx)  : ORIGIN = BOARD_FLASH_ORIGIN, LENGTH = BOARD_FLASH_LENGTH
    RAM   (xrw) : ORIGIN = BOARD_RAM_ORIGIN,   LENGTH = BOARD_RAM_LENGTH
    CCM   (rw)  : ORIGIN = BOARD_CCM_ORIGIN,   LENGTH = BOARD_CCM_LENGTH
}

/* 栈顶：RAM 最高地址，向下增长。 */
_estack = ORIGIN(RAM) + LENGTH(RAM);

/* 主栈/中断栈预留，来自 BOARD_MIN_STACK_SIZE。 */
_Min_Stack_Size = BOARD_MIN_STACK_SIZE;

SECTIONS {
    /*
     * 中断向量表。
     * KEEP 防止 --gc-sections 把它回收；必须放在 Flash 起始处，
     * 因为 Cortex-M 复位时从 0x08000000 读取初始 MSP 和 Reset_Handler。
     */
    .isr_vector : {
        . = ALIGN(4);
        KEEP(*(.isr_vector))
        . = ALIGN(4);
    } > FLASH

    /*
     * 代码和只读数据。
     * .text*        - 函数；
     * .rodata*      - 常量、字符串、只读表；
     * .init/.fini   - C 运行时初始化钩子；
     * .rti_fn*      - RT-Thread 的自动初始化段（INIT_BOARD_EXPORT 等），
     *                 按链接顺序排序后由 rt_components_init 遍历。
     */
    .text : {
        . = ALIGN(4);
        *(.text*)
        *(.rodata*)
        KEEP(*(.init))
        KEEP(*(.fini))

        . = ALIGN(4);
        __rt_init_start = .;
        KEEP(*(SORT(.rti_fn*)))
        __rt_init_end = .;

        . = ALIGN(4);
        _etext = .;
    } > FLASH

    /* ARM 异常表：C++ 异常/栈回溯使用；C 项目保留以兼容 libgcc。 */
    .ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) } > FLASH
    .ARM.exidx : {
        __exidx_start = .;
        *(.ARM.exidx*)
        __exidx_end = .;
    } > FLASH

    /* C 运行时初始化/析构数组；newlib/crt0 会遍历这些段。 */
    .preinit_array (READONLY) : {
        PROVIDE_HIDDEN(__preinit_array_start = .);
        KEEP(*(.preinit_array*))
        PROVIDE_HIDDEN(__preinit_array_end = .);
    } > FLASH

    .init_array (READONLY) : {
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array*))
        PROVIDE_HIDDEN(__init_array_end = .);
    } > FLASH

    .fini_array (READONLY) : {
        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(SORT(.fini_array.*)))
        KEEP(*(.fini_array*))
        PROVIDE_HIDDEN(__fini_array_end = .);
    } > FLASH

    /*
     * 已初始化数据。
     * 运行期位于 RAM，但初值存放在 Flash（AT > FLASH）；
     * 启动文件会用 _sidata -> _sdata 拷贝这段内容。
     */
    _sidata = LOADADDR(.data);
    .data : {
        . = ALIGN(4);
        _sdata = .;
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } > RAM AT > FLASH

    /*
     * 未初始化数据。
     * NOLOAD 表示不占 Flash；启动文件会把 _sbss.._ebss 清零。
     * 8 字节对齐满足 double/64 位访问。
     */
    .bss (NOLOAD) : {
        . = ALIGN(8);
        _sbss = .;
        __bss_start__ = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(8);
        _ebss = .;
        __bss_end__ = .;
    } > RAM

    /*
     * 主栈/中断栈预留区。
     * 这里只推进地址，不实际分配内容；MSP 从 _estack 向下增长，
     * 只要 _ebss + 4KB <= _estack 就不会压到 .bss。
     * end/_end 供 RT-Thread 堆初始化使用（当前 RT_USING_HEAP=n）。
     */
    .heap_stack (NOLOAD) : {
        . = ALIGN(8);
        PROVIDE(end = .);
        PROVIDE(_end = .);
        . = . + _Min_Stack_Size;
    } > RAM

    /* 编译器属性段，不加载到目标内存。 */
    .ARM.attributes 0 : { *(.ARM.attributes) }
}

/* 链接期检查：RAM 必须至少容纳 .bss + 4KB 栈。 */
ASSERT(_ebss + _Min_Stack_Size <= _estack, "RAM overflow: no room for main/ISR stack")
