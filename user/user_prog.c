/*
 * user/user_prog.c — 用户态测试程序
 *
 * 这段代码将在 EL0 (用户态) 运行。
 * 由于运行在非特权级别, 不能直接访问硬件,
 * 所有 I/O 操作必须通过系统调用 (SVC #0) 请求内核完成。
 *
 * 系统调用约定 (ARM64):
 *   x8     — 系统调用号
 *   x0-x5  — 参数
 *   x0     — 返回值
 *   SVC #0 — 触发异常陷入内核
 *
 * 参考文档:
 *   [ARM-ARM] C6.2.24 "SVC, Supervisor Call"
 *   [LINUX-SYSCALL] Linux ARM64 syscall ABI
 */

#include "syscall.h"  /* 系统调用号定义 */

/* ==================================================================
 * syscall_putc() — 用户态封装: 输出单个字符
 *
 * 将参数放入 x0, 系统调用号 SYS_PUTC 放入 x8,
 * 然后执行 SVC #0 触发异常。
 *
 * 内联汇编说明:
 *   "mov x0, %[ch]"   — 将字符值放入 x0 (第 1 参数)
 *   "mov x8, %[nr]"   — 将系统调用号放入 x8
 *   "svc #0"           — 触发 Supervisor Call 异常
 *
 * 参考: [ARM-ARM] C6.2.24 "SVC"
 *        GCC 手册 "Extended Asm - Assembler Instructions with C Expression Operands"
 * ================================================================== */
static void syscall_putc(char c)
{
    __asm__ volatile(
        "mov x0, %[ch]\n"          /* x0 = 字符参数 */
        "mov x8, %[nr]\n"          /* x8 = 系统调用号 */
        "svc #0\n"                 /* 触发异常 → 陷入内核 */
        :                          /* 无输出操作数 */
        : [ch] "r" ((unsigned long)c),       /* 输入: 字符值 */
          [nr] "i" (SYS_PUTC)               /* 输入: 立即数系统调用号 */
        : "x0", "x8", "memory"    /* 破坏列表: 告诉编译器这些寄存器被修改 */
    );
}

/* ==================================================================
 * syscall_exit() — 用户态封装: 进程退出
 *
 * 参考: [ARM-ARM] C6.2.24 "SVC"
 * ================================================================== */
static void syscall_exit(int status)
{
    __asm__ volatile(
        "mov x0, %[st]\n"          /* x0 = 退出码 */
        "mov x8, %[nr]\n"          /* x8 = SYS_EXIT */
        "svc #0\n"                 /* 陷入内核 */
        :
        : [st] "r" ((unsigned long)status),
          [nr] "i" (SYS_EXIT)
        : "x0", "x8", "memory"
    );
}

/* ==================================================================
 * user_puts() — 用户态封装: 输出字符串
 *
 * 逐字符调用 syscall_putc, 每个字符触发一次 SVC。
 * (低效但简单, 后续可用 SYS_WRITE 批量写入)
 * ================================================================== */
static void user_puts(const char *s)
{
    while (*s) {
        if (*s == '\n')
            syscall_putc('\r');     /* CR/LF 转换 */
        syscall_putc(*s);
        s++;
    }
}

/* ==================================================================
 * user_entry() — 用户态程序入口
 *
 * 此函数由内核通过 proc_create() 注册,
 * 经 kthread_entry() → ret_to_user() → ERET 后在 EL0 执行。
 *
 * 当前只是一个简单的演示: 打印消息然后退出。
 * ================================================================== */
void user_entry(void)
{
    user_puts("[user] Hello from EL0 (user mode)!\n");
    user_puts("[user] This message was sent via SVC syscalls.\n");
    user_puts("[user] Exiting...\n");
    syscall_exit(0);
}
