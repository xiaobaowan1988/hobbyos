/*
 * boot/vectors.S — ARM64 异常向量表
 *
 * ARM64 处理器在发生异常 (中断、系统调用、内存错误等) 时,
 * 会根据异常类型跳转到异常向量表中对应的入口。
 *
 * 异常向量表必须按 2048 字节 (0x800) 对齐, 表中共 16 个入口,
 * 每个入口占 128 字节 (0x80, 可容纳 32 条指令)。
 *
 * 参考文档:
 *   [ARM-ARM] ARM Architecture Reference Manual ARMv8-A (DDI 0487)
 *   [ARM-ARM] D1.10 "异常向量表"
 *   [ARM-ARM] D1.10.2 "VBAR_ELx 寄存器"
 */

/* ================================================================
 *  宏: SAVE_REGS — 保存所有通用寄存器到栈上
 *
 *  异常发生时, CPU 只自动保存 PC(→ELR_EL1) 和 PSTATE(→SPSR_EL1)。
 *  通用寄存器 x0-x30 必须由软件手动保存, 否则异常处理会破坏
 *  被中断代码的寄存器内容。
 *
 *  栈帧布局 (共 34×8 = 272 字节):
 *    SP+0x000: x0, x1
 *    SP+0x010: x2, x3
 *    ...
 *    SP+0x0F0: x30, xzr(padding)
 *    SP+0x100: ELR_EL1
 *    SP+0x108: SPSR_EL1
 *
 *  参考: [ARM-ARM] C6.2.27  "STP, Store Pair of Registers"
 *         [ARM-ARM] D13.2.37 "ELR_EL1, Exception Link Register"
 *         [ARM-ARM] C5.2.18  "SPSR_EL1, Saved Program Status Register"
 * ================================================================ */
.macro SAVE_REGS
    /* 预留 272 字节栈空间 (34 个 8 字节槽位)
     * SP 必须保持 16 字节对齐 (ARM64 ABI 要求)
     * 参考: [ARM-ARM] D1.8.2 "栈对齐" */
    sub     sp, sp, #272

    /* 保存通用寄存器 x0-x30 (成对存储, 每对 16 字节)
     * STP: Store Pair — 一条指令存储两个 64 位寄存器
     * 参考: [ARM-ARM] C6.2.27 "STP" */
    stp     x0,  x1,  [sp, #0x00]   /* 保存 x0, x1  到 [SP+0x00]  */
    stp     x2,  x3,  [sp, #0x10]   /* 保存 x2, x3  到 [SP+0x10]  */
    stp     x4,  x5,  [sp, #0x20]   /* 保存 x4, x5  到 [SP+0x20]  */
    stp     x6,  x7,  [sp, #0x30]   /* 保存 x6, x7  到 [SP+0x30]  */
    stp     x8,  x9,  [sp, #0x40]   /* 保存 x8, x9  到 [SP+0x40]  */
    stp     x10, x11, [sp, #0x50]   /* 保存 x10, x11 到 [SP+0x50] */
    stp     x12, x13, [sp, #0x60]   /* 保存 x12, x13 到 [SP+0x60] */
    stp     x14, x15, [sp, #0x70]   /* 保存 x14, x15 到 [SP+0x70] */
    stp     x16, x17, [sp, #0x80]   /* 保存 x16, x17 到 [SP+0x80] */
    stp     x18, x19, [sp, #0x90]   /* 保存 x18, x19 到 [SP+0x90] */
    stp     x20, x21, [sp, #0xA0]   /* 保存 x20, x21 到 [SP+0xA0] */
    stp     x22, x23, [sp, #0xB0]   /* 保存 x22, x23 到 [SP+0xB0] */
    stp     x24, x25, [sp, #0xC0]   /* 保存 x24, x25 到 [SP+0xC0] */
    stp     x26, x27, [sp, #0xD0]   /* 保存 x26, x27 到 [SP+0xD0] */
    stp     x28, x29, [sp, #0xE0]   /* 保存 x28, x29 到 [SP+0xE0] */
    str     x30,      [sp, #0xF0]   /* 保存 x30 (LR) 到 [SP+0xF0] */

    /* 保存异常状态寄存器:
     * ELR_EL1 — 异常发生时的 PC (返回地址)
     * SPSR_EL1 — 异常发生时的 PSTATE (处理器状态)
     * 参考: [ARM-ARM] D13.2.37 "ELR_EL1"
     *        [ARM-ARM] C5.2.18  "SPSR_EL1" */
    mrs     x0, elr_el1              /* 读取异常返回地址 */
    mrs     x1, spsr_el1             /* 读取保存的处理器状态 */
    stp     x0, x1, [sp, #0x100]     /* 保存到栈帧末尾 */
.endm

/* ================================================================
 *  宏: RESTORE_REGS — 从栈上恢复所有通用寄存器
 *
 *  与 SAVE_REGS 相反, 在异常处理完成后恢复现场。
 *
 *  参考: [ARM-ARM] C6.2.11 "LDP, Load Pair of Registers"
 * ================================================================ */
.macro RESTORE_REGS
    /* 恢复 ELR_EL1 和 SPSR_EL1
     * 参考: [ARM-ARM] D13.2.37, C5.2.18 */
    ldp     x0, x1, [sp, #0x100]     /* 从栈帧末尾加载 */
    msr     elr_el1, x0              /* 恢复异常返回地址 */
    msr     spsr_el1, x1             /* 恢复处理器状态 */

    /* 恢复通用寄存器 x0-x30
     * LDP: Load Pair — 一条指令加载两个 64 位寄存器
     * 参考: [ARM-ARM] C6.2.11 "LDP" */
    ldp     x0,  x1,  [sp, #0x00]   /* 恢复 x0, x1  */
    ldp     x2,  x3,  [sp, #0x10]   /* 恢复 x2, x3  */
    ldp     x4,  x5,  [sp, #0x20]   /* 恢复 x4, x5  */
    ldp     x6,  x7,  [sp, #0x30]   /* 恢复 x6, x7  */
    ldp     x8,  x9,  [sp, #0x40]   /* 恢复 x8, x9  */
    ldp     x10, x11, [sp, #0x50]   /* 恢复 x10, x11 */
    ldp     x12, x13, [sp, #0x60]   /* 恢复 x12, x13 */
    ldp     x14, x15, [sp, #0x70]   /* 恢复 x14, x15 */
    ldp     x16, x17, [sp, #0x80]   /* 恢复 x16, x17 */
    ldp     x18, x19, [sp, #0x90]   /* 恢复 x18, x19 */
    ldp     x20, x21, [sp, #0xA0]   /* 恢复 x20, x21 */
    ldp     x22, x23, [sp, #0xB0]   /* 恢复 x22, x23 */
    ldp     x24, x25, [sp, #0xC0]   /* 恢复 x24, x25 */
    ldp     x26, x27, [sp, #0xD0]   /* 恢复 x26, x27 */
    ldp     x28, x29, [sp, #0xE0]   /* 恢复 x28, x29 */
    ldr     x30,      [sp, #0xF0]   /* 恢复 x30 (LR) */

    /* 释放栈空间 */
    add     sp, sp, #272
.endm

/* ==================================================================
 *  宏: VECTOR_ENTRY — 异常向量入口模板
 *
 *  每个向量入口占 128 字节 (0x80)。此宏会:
 *    1. 对齐到 0x80 边界
 *    2. 保存寄存器
 *    3. 设置 C 函数参数 (x0 = exception_type, x1 = SP = 寄存器帧指针)
 *    4. 调用 C 异常处理函数 exception_handler()
 *    5. 恢复寄存器
 *    6. 执行 ERET 返回
 *
 *  参数:
 *    type — 异常类型编号, 传给 C 处理函数以区分异常来源
 *
 *  参考: [ARM-ARM] D1.10 "Exception vector table"
 *         [ARM-ARM] C6.2.1 "ERET"
 * ================================================================== */
.macro VECTOR_ENTRY type
    .balign 0x80                     /* 对齐到 128 字节边界 */
    SAVE_REGS                        /* 保存所有寄存器到栈 */
    mov     x0, #\type               /* x0 = 异常类型编号 (C 函数第一参数) */
    mov     x1, sp                   /* x1 = 寄存器帧指针 (C 函数第二参数) */
    bl      exception_handler        /* 调用 C 异常处理函数 */
    RESTORE_REGS                     /* 恢复所有寄存器 */
    eret                             /* 异常返回 (恢复 PC 和 PSTATE) */
.endm

/* ==================================================================
 *  异常向量表
 *
 *  ARM64 的异常向量表由 4 组 × 4 个入口 = 16 个入口组成。
 *  必须对齐到 2048 字节 (0x800) 边界。
 *
 *  四组分别对应不同的异常来源:
 *    组 0 (0x000-0x1FF): 当前 EL, 使用 SP_EL0   (内核不常用)
 *    组 1 (0x200-0x3FF): 当前 EL, 使用 SP_ELx   (内核态异常)
 *    组 2 (0x400-0x5FF): 低一级 EL, AArch64      (用户态异常/系统调用)
 *    组 3 (0x600-0x7FF): 低一级 EL, AArch32      (32位兼容, 本OS不支持)
 *
 *  每组中的 4 个入口对应:
 *    +0x00: 同步异常 (Synchronous)  — SVC/数据访问错误/指令错误等
 *    +0x80: IRQ 中断
 *   +0x100: FIQ 中断
 *   +0x180: SError (系统错误)
 *
 *  参考: [ARM-ARM] D1.10.2 "Exception vectors"
 *         [ARM-ARM] Table D1-7 "Exception vector offsets"
 * ================================================================== */

.section .text
.balign 0x800                        /* 向量表必须 2048 字节对齐 */
.globl exception_vector_table        /* 导出符号, 供 C 代码设置 VBAR_EL1 */
exception_vector_table:

/* ------------------------------------------------------------------
 * 组 0: 当前 EL, 使用 SP_EL0 (偏移 0x000 - 0x1FF)
 * 正常内核运行使用 SP_EL1, 此组通常不会被触发。
 * ------------------------------------------------------------------ */
    VECTOR_ENTRY 0                   /* 0x000: 同步异常, 当前EL, SP_EL0 */
    VECTOR_ENTRY 1                   /* 0x080: IRQ,     当前EL, SP_EL0 */
    VECTOR_ENTRY 2                   /* 0x100: FIQ,     当前EL, SP_EL0 */
    VECTOR_ENTRY 3                   /* 0x180: SError,  当前EL, SP_EL0 */

/* ------------------------------------------------------------------
 * 组 1: 当前 EL, 使用 SP_ELx (偏移 0x200 - 0x3FF)
 * 内核自身触发的异常 (如内核态缺页、内核 bug 等) 进入此组。
 * ------------------------------------------------------------------ */
    VECTOR_ENTRY 4                   /* 0x200: 同步异常, 当前EL, SP_ELx */
    VECTOR_ENTRY 5                   /* 0x280: IRQ,     当前EL, SP_ELx */
    VECTOR_ENTRY 6                   /* 0x300: FIQ,     当前EL, SP_ELx */
    VECTOR_ENTRY 7                   /* 0x380: SError,  当前EL, SP_ELx */

/* ------------------------------------------------------------------
 * 组 2: 低一级 EL (EL0), AArch64 (偏移 0x400 - 0x5FF)
 * 用户态程序触发的异常 (系统调用 SVC、缺页等) 进入此组。
 * 这是后续章节实现 syscall 的关键入口。
 * ------------------------------------------------------------------ */
    VECTOR_ENTRY 8                   /* 0x400: 同步异常, 低EL, AArch64 */
    VECTOR_ENTRY 9                   /* 0x480: IRQ,     低EL, AArch64 */
    VECTOR_ENTRY 10                  /* 0x500: FIQ,     低EL, AArch64 */
    VECTOR_ENTRY 11                  /* 0x580: SError,  低EL, AArch64 */

/* ------------------------------------------------------------------
 * 组 3: 低一级 EL (EL0), AArch32 (偏移 0x600 - 0x7FF)
 * 32 位用户态程序的异常, 本 OS 不支持 AArch32。
 * ------------------------------------------------------------------ */
    VECTOR_ENTRY 12                  /* 0x600: 同步异常, 低EL, AArch32 */
    VECTOR_ENTRY 13                  /* 0x680: IRQ,     低EL, AArch32 */
    VECTOR_ENTRY 14                  /* 0x700: FIQ,     低EL, AArch32 */
    VECTOR_ENTRY 15                  /* 0x780: SError,  低EL, AArch32 */
