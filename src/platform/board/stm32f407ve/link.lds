/*
 * v2 STM32F407VE 链接脚本（GNU ld）
 *
 * 这个文件是 GCC 侧的内存事实，与 board_config.h 的 BOARD_*_ORIGIN/
 * SIZE_KB 必须完全一致。两者无法共享 C 宏，因此由
 * tools/tests/test_static_board.py 做文本交叉校验：
 *   - FLASH 的 LENGTH = BOARD_CODE_FLASH_SIZE_KB；
 *   - NVM/RAM/CCM 的 ORIGIN/LENGTH = board_config.h 对应常量；
 *   - _Min_Stack_Size = BOARD_MIN_STACK_SIZE。
 * 只改一边会被 Host CTest 立即拒绝。
 *
 * 存储器：
 *   FLASH 256KB @ 0x08000000 - 中断向量、代码、只读数据、.data 初值。
 *                    注意不是芯片总 Flash 512KB：尾部 256KB 留给 NVM，
 *                    绝不能链接进固件，否则参数区会被代码覆盖。
 *   NVM   256KB @ 0x08040000 - 参数 A/B 槽（各 128KB 扇区）。
 *                    这里声明为 MEMORY 区域只是为了让 map 文件可见；
 *                    SECTIONS 中没有任何段使用它，固件也不会写入。
 *   RAM   128KB @ 0x20000000 - .data、.bss、主栈/中断栈、RT-Thread
 *                    静态对象和所有 DMA 缓冲区。
 *   CCM    64KB @ 0x10000000 - 故意不使用：F407 的 DMA 不能访问 CCM。
 *                    为了保持“所有缓冲区都可 DMA”的一致规则，统一
 *                    放主 SRAM；当前 CCM 只出现在 map 中，Used=0。
 *
 * 符号约定（启动文件/RT-Thread 会引用）：
 *   _estack       - 栈顶（RAM 末尾），复位时装入 MSP；
 *   _sidata       - .data 在 Flash 中的加载地址；
 *   _sdata/_edata - .data 在 RAM 中的起止；
 *   _sbss/_ebss   - .bss 起止；
 *   end/_end      - 堆起点（RT_USING_HEAP 时由 platform boot 使用）。
 *
 * 修改板级内存布局时：
 *   1. 改 board_config.h 的 BOARD_*_ORIGIN/SIZE_KB；
 *   2. 改本文件 MEMORY 与 _Min_Stack_Size；
 *   3. 运行 python tools/fwconfig.py gen-keil --profile <name>
 *      （Keil IROM/IRAM/Stack 自动派生）；
 *   4. 运行 Host CTest，确认静态板级一致性测试通过。
 */
ENTRY(Reset_Handler)

MEMORY {
    /*
     * FLASH 只覆盖代码区。NVM 从 0x08040000 开始，和 FLASH 区域
     * 首尾相接但不重叠。
     */
    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 256K
    /* 参数区：只声明、不分配段；stm32f407_nvm.c 用绝对地址访问。 */
    NVM   (r)   : ORIGIN = 0x08040000, LENGTH = 256K
    /* 主 SRAM：可执行/可读写；DMA 只能访问这里，不能访问 CCM。 */
    RAM   (xrw) : ORIGIN = 0x20000000, LENGTH = 128K
    /* CCM：当前不链接任何段；保留声明便于以后放非 DMA 数据。 */
    CCM   (rw)  : ORIGIN = 0x10000000, LENGTH = 64K
}

/* 栈顶：RAM 最高地址，向下增长。 */
_estack = ORIGIN(RAM) + LENGTH(RAM);

/* 主栈/中断栈预留，对应 board_config.h 的 BOARD_MIN_STACK_SIZE。 */
_Min_Stack_Size = 4096;

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
