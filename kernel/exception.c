/*
 * kernel/exception.c — 异常处理 (C 层)
 *
 * 所有异常 (同步异常、IRQ、FIQ、SError) 最终都会从汇编向量入口
 * 调用到这里的 exception_handler() 函数。
 *
 * 参考文档:
 *   [ARM-ARM] ARM Architecture Reference Manual ARMv8-A (DDI 0487)
 *   [ARM-ARM] D1.10  "异常向量表"
 *   [ARM-ARM] D13.2  "AArch64 系统寄存器"
 */

#include "uart.h"

/* ==================================================================
 * 异常类型名称表
 *
 * 对应 vectors.S 中 VECTOR_ENTRY 宏的 type 参数 (0-15)。
 * 共 4 组 × 4 种 = 16 个入口。
 *
 * 参考: [ARM-ARM] D1.10.2 Table D1-7 "Exception vector offsets"
 * ================================================================== */
static const char *exception_names[] = {
    /* 组 0: 当前 EL, SP_EL0 */
    "Sync  (curr EL, SP_EL0)",    /* 0  — 同步异常 */
    "IRQ   (curr EL, SP_EL0)",    /* 1  — IRQ 中断 */
    "FIQ   (curr EL, SP_EL0)",    /* 2  — FIQ 快速中断 */
    "SError(curr EL, SP_EL0)",    /* 3  — 系统错误 */

    /* 组 1: 当前 EL, SP_ELx (内核态异常) */
    "Sync  (curr EL, SP_ELx)",    /* 4  — 内核同步异常 */
    "IRQ   (curr EL, SP_ELx)",    /* 5  — 内核 IRQ */
    "FIQ   (curr EL, SP_ELx)",    /* 6  — 内核 FIQ */
    "SError(curr EL, SP_ELx)",    /* 7  — 内核系统错误 */

    /* 组 2: 低 EL, AArch64 (用户态异常) */
    "Sync  (lower EL, A64)",      /* 8  — 用户态同步 (含 SVC) */
    "IRQ   (lower EL, A64)",      /* 9  — 用户态 IRQ */
    "FIQ   (lower EL, A64)",      /* 10 — 用户态 FIQ */
    "SError(lower EL, A64)",      /* 11 — 用户态系统错误 */

    /* 组 3: 低 EL, AArch32 */
    "Sync  (lower EL, A32)",      /* 12 */
    "IRQ   (lower EL, A32)",      /* 13 */
    "FIQ   (lower EL, A32)",      /* 14 */
    "SError(lower EL, A32)",      /* 15 */
};

/* ==================================================================
 * 辅助函数: 将 64 位无符号整数转为 16 进制字符串
 *
 * 由于我们运行在裸机环境, 没有 printf/sprintf,
 * 需要手动实现数字到字符串的转换。
 *
 * 参数:
 *   buf — 输出缓冲区, 至少 17 字节 (16 个 hex 字符 + '\0')
 *   val — 要转换的 64 位值
 * ================================================================== */
static void u64_to_hex(char *buf, unsigned long val)
{
    /* 16 进制字符映射表 */
    const char hex[] = "0123456789abcdef";

    /* 从高位到低位依次提取每个 4-bit nibble
     * 一个 64 位值有 16 个 nibble (16 × 4 = 64) */
    for (int i = 15; i >= 0; i--) {
        buf[i] = hex[val & 0xF];    /* 取最低 4 位转为 hex 字符 */
        val >>= 4;                  /* 右移 4 位, 处理下一个 nibble */
    }
    buf[16] = '\0';                 /* null 终止符 */
}

/* ==================================================================
 * 辅助函数: 打印 "标签: 0x值" 格式的寄存器信息
 * ================================================================== */
static void print_reg(const char *name, unsigned long val)
{
    char buf[17];                   /* 16 hex chars + '\0' */
    uart_puts(name);                /* 打印寄存器名 */
    uart_puts(": 0x");              /* 打印前缀 */
    u64_to_hex(buf, val);           /* 转换为十六进制字符串 */
    uart_puts(buf);                 /* 打印值 */
    uart_puts("\n");                /* 换行 */
}

/* ==================================================================
 * exception_handler() — 统一异常处理入口 (C 层)
 *
 * 由汇编向量入口 (vectors.S VECTOR_ENTRY) 调用。
 *
 * 参数:
 *   type  — 异常类型编号 (0-15), 对应向量表的 16 个入口
 *   frame — 指向栈上保存的寄存器帧:
 *           frame[0..30] = x0-x30
 *           frame[32]    = ELR_EL1 (异常返回地址)
 *           frame[33]    = SPSR_EL1 (保存的处理器状态)
 *
 * 当前实现: 打印异常信息后死循环 (panic)。
 * 后续章节将为特定异常 (如 IRQ、SVC) 添加专门处理。
 *
 * 参考: [ARM-ARM] D13.2.37 "ELR_EL1"
 *        [ARM-ARM] C5.2.18  "SPSR_EL1"
 *        [ARM-ARM] D13.2.36 "ESR_EL1, Exception Syndrome Register"
 *        [ARM-ARM] D13.2.39 "FAR_EL1, Fault Address Register"
 * ================================================================== */
void exception_handler(unsigned long type, unsigned long *frame)
{
    /* 读取异常综合征寄存器 (ESR_EL1)
     *
     * ESR_EL1 包含异常的详细原因:
     *   bits[31:26] — EC (Exception Class): 异常类别编码
     *     常见值:
     *       0x15 = SVC (AArch64 系统调用)
     *       0x20 = 指令访问异常 (Instruction Abort, 低 EL)
     *       0x21 = 指令访问异常 (当前 EL)
     *       0x24 = 数据访问异常 (Data Abort, 低 EL)
     *       0x25 = 数据访问异常 (当前 EL)
     *   bits[24:0]  — ISS (Instruction Specific Syndrome): 详细信息
     *
     * 参考: [ARM-ARM] D13.2.36 "ESR_EL1, Exception Syndrome Register"
     *        [ARM-ARM] D1.10.4  "异常综合征编码" */
    unsigned long esr;
    __asm__ volatile("mrs %0, esr_el1" : "=r"(esr));

    /* 读取故障地址寄存器 (FAR_EL1)
     *
     * FAR_EL1 包含引起数据/指令访问异常的虚拟地址。
     * 对于 SVC 等同步异常, FAR_EL1 的值无意义。
     *
     * 参考: [ARM-ARM] D13.2.39 "FAR_EL1, Fault Address Register" */
    unsigned long far;
    __asm__ volatile("mrs %0, far_el1" : "=r"(far));

    /* 打印异常信息 */
    uart_puts("\n!!! EXCEPTION: ");
    if (type < 16)
        uart_puts(exception_names[type]);
    uart_puts("\n");

    /* 打印关键寄存器值 */
    print_reg("  ESR_EL1  ", esr);   /* 异常综合征 */
    print_reg("  ELR_EL1  ", frame[32]); /* 异常返回地址 (触发异常的指令地址) */
    print_reg("  SPSR_EL1 ", frame[33]); /* 保存的处理器状态 */
    print_reg("  FAR_EL1  ", far);   /* 故障地址 */

    /* 打印 ESR 的 EC 字段含义 */
    unsigned long ec = (esr >> 26) & 0x3F;
    uart_puts("  EC = 0x");
    char ecbuf[3];
    ecbuf[0] = "0123456789abcdef"[(ec >> 4) & 0xF];
    ecbuf[1] = "0123456789abcdef"[ec & 0xF];
    ecbuf[2] = '\0';
    uart_puts(ecbuf);
    uart_puts(" → ");

    /* 解码 EC (Exception Class)
     * 参考: [ARM-ARM] Table D1-8 "Exception class encoding" */
    switch (ec) {
    case 0x00: uart_puts("Unknown reason\n"); break;
    case 0x01: uart_puts("WFI/WFE trapped\n"); break;
    case 0x0E: uart_puts("Illegal execution state\n"); break;
    case 0x15: uart_puts("SVC (AArch64 system call)\n"); break;
    case 0x20: uart_puts("Instruction Abort (lower EL)\n"); break;
    case 0x21: uart_puts("Instruction Abort (same EL)\n"); break;
    case 0x22: uart_puts("PC alignment fault\n"); break;
    case 0x24: uart_puts("Data Abort (lower EL)\n"); break;
    case 0x25: uart_puts("Data Abort (same EL)\n"); break;
    case 0x26: uart_puts("SP alignment fault\n"); break;
    case 0x2C: uart_puts("FP/SIMD trapped\n"); break;
    case 0x30: uart_puts("Breakpoint (lower EL)\n"); break;
    case 0x31: uart_puts("Breakpoint (same EL)\n"); break;
    case 0x32: uart_puts("Software Step (lower EL)\n"); break;
    case 0x33: uart_puts("Software Step (same EL)\n"); break;
    default:   uart_puts("(other)\n"); break;
    }

    /* 死循环 — 当前不尝试恢复, 直接 panic
     * 后续章节会为可恢复异常 (IRQ, SVC) 添加正常返回路径 */
    uart_puts("\n*** KERNEL PANIC — halting ***\n");
    while (1)
        __asm__ volatile("wfe");
}
