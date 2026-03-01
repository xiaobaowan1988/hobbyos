/*
 * kernel/switch.S — 进程上下文切换与用户态跳转
 *
 * 包含两个关键汇编函数:
 *   switch_to()   — 保存/恢复 callee-saved 寄存器, 实现进程切换
 *   ret_to_user() — 通过 ERET 从 EL1 (内核) 跳转到 EL0 (用户态)
 *
 * 参考文档:
 *   [ARM-ARM] ARM Architecture Reference Manual ARMv8-A (DDI 0487)
 *   [ARM-ABI] AAPCS64 (ARM 64-bit Architecture Procedure Call Standard)
 */

.section .text

/* ==================================================================
 *  switch_to(struct cpu_context *prev, struct cpu_context *next)
 *
 *  保存当前进程的 callee-saved 寄存器到 prev 指向的 context 结构,
 *  然后从 next 指向的 context 恢复寄存器。
 *
 *  这是进程切换的核心: 执行到 ret 指令时,
 *  LR 已被恢复为 next 进程上次挂起时的返回地址,
 *  因此 ret 会跳转到 next 进程继续执行。
 *
 *  cpu_context 结构布局 (每个字段 8 字节):
 *    偏移 0x00: x19
 *    偏移 0x08: x20
 *    偏移 0x10: x21
 *    偏移 0x18: x22
 *    偏移 0x20: x23
 *    偏移 0x28: x24
 *    偏移 0x30: x25
 *    偏移 0x38: x26
 *    偏移 0x40: x27
 *    偏移 0x48: x28
 *    偏移 0x50: fp (x29)
 *    偏移 0x58: lr (x30)
 *    偏移 0x60: sp
 *
 *  参数:
 *    x0 — 指向 prev 进程的 cpu_context
 *    x1 — 指向 next 进程的 cpu_context
 *
 *  参考: [ARM-ABI] "5.1.1 General-purpose Registers"
 *         [ARM-ARM] C6.2.27 "STP" / C6.2.11 "LDP"
 * ================================================================== */
.globl switch_to
switch_to:
    /* ---- 保存当前进程的 callee-saved 寄存器到 [x0] ---- */

    /* 保存 x19, x20 到 prev->context 偏移 0x00
     * STP: Store Pair — 一条指令存两个 64 位寄存器
     * 参考: [ARM-ARM] C6.2.27 "STP" */
    stp     x19, x20, [x0, #0x00]

    /* 保存 x21, x22 到偏移 0x10 */
    stp     x21, x22, [x0, #0x10]

    /* 保存 x23, x24 到偏移 0x20 */
    stp     x23, x24, [x0, #0x20]

    /* 保存 x25, x26 到偏移 0x30 */
    stp     x25, x26, [x0, #0x30]

    /* 保存 x27, x28 到偏移 0x40 */
    stp     x27, x28, [x0, #0x40]

    /* 保存 fp(x29) 和 lr(x30) 到偏移 0x50
     * LR 是进程切换后的返回地址, 极其关键!
     * 参考: [ARM-ABI] "5.1.1" — x29=FP, x30=LR 为 callee-saved */
    stp     x29, x30, [x0, #0x50]

    /* 保存当前栈指针 SP 到偏移 0x60
     * 注意: SP 不能直接用于 STR 的地址, 需要先移到通用寄存器。
     * 参考: [ARM-ARM] "SP 只能用于加减法运算和 MOV" */
    mov     x9, sp                 /* x9 = 当前 SP (临时寄存器) */
    str     x9, [x0, #0x60]       /* 保存 SP */

    /* ---- 恢复下一进程的 callee-saved 寄存器从 [x1] ---- */

    /* 恢复 x19, x20 */
    ldp     x19, x20, [x1, #0x00]

    /* 恢复 x21, x22 */
    ldp     x21, x22, [x1, #0x10]

    /* 恢复 x23, x24 */
    ldp     x23, x24, [x1, #0x20]

    /* 恢复 x25, x26 */
    ldp     x25, x26, [x1, #0x30]

    /* 恢复 x27, x28 */
    ldp     x27, x28, [x1, #0x40]

    /* 恢复 fp(x29) 和 lr(x30)
     * LR 被恢复为 next 进程的挂起点地址 */
    ldp     x29, x30, [x1, #0x50]

    /* 恢复栈指针 SP */
    ldr     x9, [x1, #0x60]       /* 加载 next 的 SP 到 x9 */
    mov     sp, x9                 /* 设置 SP */

    /* 返回 — 此时 LR 已是 next 进程的值,
     * 所以 ret 实际跳转到 next 进程上次挂起的位置。
     * 对于新创建的进程, LR = kthread_entry。
     * 参考: [ARM-ARM] C6.2.15 "RET" */
    ret

/* ==================================================================
 *  ret_to_user(uint64_t pc, uint64_t sp)
 *
 *  从内核态 (EL1) 跳转到用户态 (EL0)。
 *
 *  设置:
 *    ELR_EL1  = pc  (ERET 后 CPU 跳转到此地址)
 *    SP_EL0   = sp  (用户态栈指针)
 *    SPSR_EL1 = 0   (EL0t 执行状态, DAIF 全部不屏蔽)
 *
 *  然后执行 ERET 完成从 EL1 → EL0 的转换。
 *
 *  参数:
 *    x0 — 用户态入口地址 (EL0 PC)
 *    x1 — 用户态栈指针 (SP_EL0)
 *
 *  参考: [ARM-ARM] D13.2.37 "ELR_EL1"
 *         [ARM-ARM] C5.2.18  "SPSR_EL1"
 *         [ARM-ARM] D1.6.1   "EL0t 执行状态"
 *         [ARM-ARM] C6.2.1   "ERET"
 * ================================================================== */
.globl ret_to_user
ret_to_user:
    /* 设置异常返回地址 = 用户态入口 PC
     * ERET 后 CPU 的 PC 将变为 ELR_EL1 的值。
     * 参考: [ARM-ARM] D13.2.37 "ELR_EL1" */
    msr     elr_el1, x0

    /* 设置用户态栈指针 SP_EL0
     * EL0 使用 SP_EL0 作为栈指针 (不是 SP_EL1)。
     * 参考: [ARM-ARM] D1.8.2 "SP_EL0" */
    msr     sp_el0, x1

    /* 设置 SPSR_EL1 = 0x00000000
     * 这意味着:
     *   bits[3:0] = 0b0000 → EL0t (用户态, 使用 SP_EL0)
     *   bits[9:6] = 0b0000 → DAIF 全部清除 (允许中断)
     *
     * ERET 时 PSTATE 将从 SPSR_EL1 恢复。
     * 参考: [ARM-ARM] C5.2.18 "SPSR_EL1"
     *        [ARM-ARM] D1.6.1 "EL0t" */
    mov     x2, #0
    msr     spsr_el1, x2

    /* 执行异常返回: EL1 → EL0
     * ERET 同时完成:
     *   1. PC ← ELR_EL1 (跳转到用户态代码)
     *   2. PSTATE ← SPSR_EL1 (切换到 EL0)
     *
     * 参考: [ARM-ARM] C6.2.1 "ERET, Exception Return" */
    eret
