/*
 * boot/vectors.S — ARM64 异常向量表
 *
 * ARM64 处理器在发生异常 (中断、系统调用、内存错误等) 时,
 * 会根据异常类型跳转到异常向量表中对应的入口。
 *
 * 异常向量表必须按 2048 字节 (0x800) 对齐, 表中共 16 个入口,
 * 每个入口占 128 字节 (0x80, 可容纳 32 条指令)。
 *
 * 重要: SAVE_REGS 需要 20 条指令 (80 字节), RESTORE_REGS 也需要
 * 20 条指令 (80 字节), 加上中间的 3 条指令和 eret, 合计 176 字节,
 * 超过 128 字节限制! 因此每个向量入口只放 SAVE_REGS + 参数设置
 * + 跳转 (92 字节 < 128 字节), 实际的 C 函数调用和 RESTORE_REGS
 * 放在表外的 vector_common 中。
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
 *  共 20 条指令 = 80 字节 (0x50)
 *
 *  参考: [ARM-ARM] C6.2.27  "STP, Store Pair of Registers"
 *         [ARM-ARM] D13.2.37 "ELR_EL1, Exception Link Register"
 *         [ARM-ARM] C5.2.18  "SPSR_EL1, Saved Program Status Register"
 * ================================================================ */
.macro SAVE_REGS
    sub     sp, sp, #272
    stp     x0,  x1,  [sp, #0x00]
    stp     x2,  x3,  [sp, #0x10]
    stp     x4,  x5,  [sp, #0x20]
    stp     x6,  x7,  [sp, #0x30]
    stp     x8,  x9,  [sp, #0x40]
    stp     x10, x11, [sp, #0x50]
    stp     x12, x13, [sp, #0x60]
    stp     x14, x15, [sp, #0x70]
    stp     x16, x17, [sp, #0x80]
    stp     x18, x19, [sp, #0x90]
    stp     x20, x21, [sp, #0xA0]
    stp     x22, x23, [sp, #0xB0]
    stp     x24, x25, [sp, #0xC0]
    stp     x26, x27, [sp, #0xD0]
    stp     x28, x29, [sp, #0xE0]
    str     x30,      [sp, #0xF0]
    mrs     x0, elr_el1
    mrs     x1, spsr_el1
    stp     x0, x1, [sp, #0x100]
.endm

/* ================================================================
 *  宏: RESTORE_REGS — 从栈上恢复所有通用寄存器
 *
 *  与 SAVE_REGS 相反, 在异常处理完成后恢复现场。
 *  共 20 条指令 = 80 字节 (0x50)
 *
 *  参考: [ARM-ARM] C6.2.11 "LDP, Load Pair of Registers"
 * ================================================================ */
.macro RESTORE_REGS
    ldp     x0, x1, [sp, #0x100]
    msr     elr_el1, x0
    msr     spsr_el1, x1
    ldp     x0,  x1,  [sp, #0x00]
    ldp     x2,  x3,  [sp, #0x10]
    ldp     x4,  x5,  [sp, #0x20]
    ldp     x6,  x7,  [sp, #0x30]
    ldp     x8,  x9,  [sp, #0x40]
    ldp     x10, x11, [sp, #0x50]
    ldp     x12, x13, [sp, #0x60]
    ldp     x14, x15, [sp, #0x70]
    ldp     x16, x17, [sp, #0x80]
    ldp     x18, x19, [sp, #0x90]
    ldp     x20, x21, [sp, #0xA0]
    ldp     x22, x23, [sp, #0xB0]
    ldp     x24, x25, [sp, #0xC0]
    ldp     x26, x27, [sp, #0xD0]
    ldp     x28, x29, [sp, #0xE0]
    ldr     x30,      [sp, #0xF0]
    add     sp, sp, #272
.endm

/* ==================================================================
 *  宏: VECTOR_ENTRY — 异常向量入口模板
 *
 *  每个向量入口占 128 字节 (0x80)。此宏会:
 *    1. 对齐到 0x80 边界
 *    2. 保存寄存器 (20 条指令 = 80 字节)
 *    3. 设置 C 函数参数 (x0 = exception_type, x1 = SP = 寄存器帧指针)
 *    4. 跳转到 vector_common (表外的共享处理代码)
 *
 *  合计: 20 + 3 = 23 条指令 = 92 字节 < 128 字节 ✓
 *
 *  vector_common 负责:
 *    - 调用 C 异常处理函数 exception_handler()
 *    - 恢复寄存器 (RESTORE_REGS)
 *    - 执行 ERET 返回
 *
 *  参数:
 *    type — 异常类型编号, 传给 C 处理函数以区分异常来源
 *
 *  参考: [ARM-ARM] D1.10 "Exception vector table"
 * ================================================================== */
.macro VECTOR_ENTRY type
    .balign 0x80                     /* 对齐到 128 字节边界 */
    SAVE_REGS                        /* 保存所有寄存器到栈 (80 字节) */
    mov     x0, #\type               /* x0 = 异常类型编号 (4 字节) */
    mov     x1, sp                   /* x1 = 寄存器帧指针 (4 字节) */
    b       vector_common            /* 跳转到共享处理代码 (4 字节) */
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

/* ==================================================================
 *  vector_common — 共享的异常处理尾部
 *
 *  所有 16 个向量入口在完成 SAVE_REGS 和参数设置后,
 *  都跳转到这里。此代码位于向量表之外, 没有大小限制。
 *
 *  流程:
 *    1. bl exception_handler  — 调用 C 异常处理函数
 *    2. RESTORE_REGS          — 恢复所有寄存器
 *    3. eret                  — 异常返回 (恢复 PC 和 PSTATE)
 *
 *  参考: [ARM-ARM] C6.2.1 "ERET, Exception Return"
 * ================================================================== */
vector_common:
    bl      exception_handler        /* 调用 C 异常处理函数 */
    RESTORE_REGS                     /* 恢复所有寄存器 */
    eret                             /* 异常返回 (恢复 PC 和 PSTATE) */
