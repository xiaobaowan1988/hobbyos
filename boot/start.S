/*
 * boot/start.S — ARM64 裸机启动入口
 *
 * 本文件是整个操作系统最先执行的代码。
 * 目标平台: QEMU virt machine (AArch64 / ARMv8-A)
 *
 * 参考文档:
 *   [ARM-ARM] ARM Architecture Reference Manual ARMv8-A (DDI 0487)
 *   [QEMU-VIRT] https://www.qemu.org/docs/master/system/arm/virt.html
 */

/* ================================================================
 *  .section .text.boot — 放置在链接脚本的最前端, 确保 _start 位于
 *  镜像的 0 偏移处。QEMU virt 默认把内核加载到 0x4000_0000。
 *  参考: [QEMU-VIRT] "载入地址为 -kernel 参数指定映像的入口"
 * ================================================================ */
.section .text.boot

/* ================================================================
 *  .globl _start — 导出全局符号，链接器据此确定入口点。
 *  参考: GNU as 手册 "7.40 .globl"
 * ================================================================ */
.globl _start

/* ================================================================
 *  _start: 系统上电/复位后, CPU 从此处开始执行。
 *
 *  QEMU virt 以 EL1 (内核态) 启动 CPU, 但真实硬件可能从 EL2/EL3
 *  启动。此处先检测当前异常等级(EL), 如果不在 EL1 则切换到 EL1。
 *
 *  参考: [ARM-ARM] D1.6 "异常等级与安全状态"
 * ================================================================ */
_start:
    /* ----------------------------------------------------------
     * 读取 CurrentEL 系统寄存器, 获取当前异常等级。
     * CurrentEL 位于 bits[3:2]:
     *   0b00 = EL0,  0b01 = EL1,  0b10 = EL2,  0b11 = EL3
     *
     * 参考: [ARM-ARM] D13.2.22 "CurrentEL, Current Exception Level"
     * ---------------------------------------------------------- */
    mrs     x0, CurrentEL           // x0 = CurrentEL 寄存器值
    and     x0, x0, #0xC            // 提取 bits[3:2]  (掩码 0b1100)
    cmp     x0, #4                  // 4 = (EL1 << 2), 即 0b0100
    b.eq    1f                      // 如果已在 EL1, 跳过 EL2→EL1 切换

    /* ----------------------------------------------------------
     * 从 EL2 降级到 EL1:
     * 1) 配置 HCR_EL2 使 EL1 使用 AArch64 执行状态
     * 2) 设置 SPSR_EL2 为 EL1h (使用 SP_EL1)
     * 3) 设置 ELR_EL2 指向 1f 标签
     * 4) 执行 ERET 完成异常返回 (即降级)
     *
     * 参考: [ARM-ARM] D13.2.47 "HCR_EL2, Hypervisor Configuration Register"
     *        [ARM-ARM] C5.2.19  "SPSR_EL2, Saved Program Status Register (EL2)"
     *        [ARM-ARM] C5.2.1   "ELR_EL2, Exception Link Register (EL2)"
     *        [ARM-ARM] C6.2.1   "ERET"
     * ---------------------------------------------------------- */
    ldr     x0, =((1 << 31))       // HCR_EL2.RW = 1 → EL1 为 AArch64
    msr     hcr_el2, x0            // 写入 HCR_EL2

    mov     x0, #0x3C5             // SPSR_EL2 = EL1h | DAIF 全屏蔽
                                    //   bits[3:0]=0b0101 (EL1h, SP_EL1)
                                    //   bits[9:6]=0b1111 (DAIF 屏蔽)
                                    // 参考: [ARM-ARM] C5.2.19 SPSR_EL2
    msr     spsr_el2, x0           // 写入 SPSR_EL2

    adr     x0, 1f                 // 将标签 1 的地址放入 ELR_EL2
    msr     elr_el2, x0            // ERET 后 PC 将跳转到标签 1
    eret                           // 异常返回 → 降级到 EL1h

    /* ----------------------------------------------------------
     * 标签 1: 现在确保运行在 EL1
     * ---------------------------------------------------------- */
1:

    /* ----------------------------------------------------------
     * 多核处理: 只让主核 (CPU#0, MPIDR_EL1.Aff0 == 0) 继续,
     * 其余核进入低功耗等待循环 (WFE)。
     *
     * 参考: [ARM-ARM] D13.2.86 "MPIDR_EL1, Multiprocessor Affinity Register"
     *        [ARM-ARM] C6.2.33  "WFE, Wait For Event"
     * ---------------------------------------------------------- */
    mrs     x0, mpidr_el1          // 读取 MPIDR_EL1 (多处理器亲和寄存器)
    and     x0, x0, #0xFF          // 提取 Aff0 (最低 8 位 = 核心编号)
    cbz     x0, 2f                 // 如果是 CPU#0, 跳转到 2f 继续启动
    /* 非主核: 进入休眠循环 */
spin:
    wfe                            // 等待事件 (低功耗等待)
    b       spin                   // 循环等待

    /* ----------------------------------------------------------
     * 标签 2: 仅主核到达此处
     * ---------------------------------------------------------- */
2:

    /* ----------------------------------------------------------
     * 使能 FP/SIMD (浮点和 NEON) 访问。
     *
     * CPACR_EL1.FPEN (bits[21:20]):
     *   0b00 = 陷入 EL1 (默认, 禁止访问)
     *   0b11 = EL0 和 EL1 均可访问
     *
     * GCC -O2 可能生成 NEON 指令 (如 memset 使用 SIMD),
     * 如果 FPEN 未设置则触发 EC=0x07 陷入。
     *
     * 参考: [ARM-ARM] D13.2.20 "CPACR_EL1, Architectural Feature Access Control Register"
     * ---------------------------------------------------------- */
    mrs     x0, cpacr_el1          // 读取当前 CPACR_EL1
    orr     x0, x0, #(3 << 20)    // FPEN = 0b11 → 允许 FP/SIMD 访问
    msr     cpacr_el1, x0          // 写回 CPACR_EL1
    isb                            // 指令同步屏障: 确保新设置立即生效

    /* ----------------------------------------------------------
     * 设置栈指针 SP_EL1。
     *
     * ARM64 栈向低地址增长 (full-descending)。SP 必须指向
     * 已分配内存区域的顶部 (最高地址)。
     *
     * boot_stack_top 是 .bss 段中 16KB 启动栈的顶部。
     * 注意: QEMU virt 的 RAM 从 0x4000_0000 开始,
     * 不可将 SP 设为 0x4000_0000 (向下增长会访问 RAM 之外!)。
     *
     * 参考: [ARM-ARM] D1.8.2 "Stack Pointer selection"
     *        [ARM-ABI] AAPCS64 "6.2.2 The Stack"
     * ---------------------------------------------------------- */
    ldr     x0, =boot_stack_top    // x0 = 启动栈顶部地址 (在 BSS 段中)
    mov     sp, x0                 // SP = 栈顶 (向下增长)

    /* ----------------------------------------------------------
     * 清零 BSS 段: C 语言规范要求未初始化的全局/静态变量为 0。
     * __bss_start 和 __bss_end 由链接脚本定义。
     *
     * 参考: System V ABI "节 4.1: 对象文件" (BSS 定义)
     * ---------------------------------------------------------- */
    ldr     x0, =__bss_start       // x0 = BSS 段起始地址
    ldr     x1, =__bss_end         // x1 = BSS 段结束地址
    cmp     x0, x1                 // 如果 BSS 段为空则跳过
    b.ge    3f
bss_loop:
    str     xzr, [x0], #8          // 将 0 写入 [x0], x0 += 8
    cmp     x0, x1                 // 判断是否到达 __bss_end
    b.lt    bss_loop               // 未到达则继续循环
3:

    /* ----------------------------------------------------------
     * 设置异常向量表基地址 (VBAR_EL1)。
     *
     * VBAR_EL1 (Vector Base Address Register, EL1) 保存异常向量表
     * 的基地址。当异常发生时, CPU 根据 VBAR_EL1 + 偏移 跳转到
     * 对应的异常处理入口。
     *
     * exception_vector_table 在 boot/vectors.S 中定义。
     *
     * 参考: [ARM-ARM] D13.2.139 "VBAR_EL1, Vector Base Address Register (EL1)"
     * ---------------------------------------------------------- */
    ldr     x0, =exception_vector_table  // x0 = 异常向量表基地址
    msr     vbar_el1, x0                 // 写入 VBAR_EL1

    /* ----------------------------------------------------------
     * 跳转到 C 入口函数 kernel_main()。
     * 使用 bl 而非 b, 这样如果 kernel_main 返回可以捕获。
     *
     * 参考: [ARM-ARM] C6.2.6 "BL, Branch with Link"
     * ---------------------------------------------------------- */
    bl      kernel_main            // 调用 C 函数 kernel_main()

    /* ----------------------------------------------------------
     * 如果 kernel_main 意外返回, 进入死循环防止 CPU 执行随机内存。
     * ---------------------------------------------------------- */
halt:
    wfe                            // 等待事件 (低功耗)
    b       halt                   // 永远循环

/* ==================================================================
 * 启动栈 — 在 .bss 段中分配 16KB
 *
 * ARM64 栈为 full-descending: SP 初始指向 boot_stack_top,
 * 每次 push (如 stp) 先减小 SP 再写入。
 * 因此 boot_stack 是最低地址, boot_stack_top 是最高地址。
 *
 * 大小 16KB (4 页) 对于启动阶段足够:
 *   - kernel_main() 调用链深度有限
 *   - 后续各进程会分配独立的内核栈
 *
 * .align 4 = 16 字节对齐 (2^4 = 16), 满足 AAPCS64 要求:
 *   "SP 在公共接口处必须 16 字节对齐"
 *
 * 参考: [ARM-ABI] AAPCS64 "6.2.2 The Stack" — SP 16-byte alignment
 * ================================================================== */
.section .bss
.align 4                           // 16 字节对齐 (2^4)
boot_stack:
    .space 16384                   // 16KB = 4 pages
boot_stack_top:
