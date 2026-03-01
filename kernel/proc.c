/*
 * kernel/proc.c — 进程管理
 *
 * 实现进程控制块管理、进程创建和用户态跳转。
 *
 * 参考文档:
 *   [ARM-ARM] ARM Architecture Reference Manual ARMv8-A (DDI 0487)
 *   [OSTEP] Operating Systems: Three Easy Pieces
 */

#include "proc.h"
#include "mm.h"
#include "uart.h"

/* ==================================================================
 * 进程表 — 所有 PCB 的静态数组
 *
 * 使用固定大小数组 (MAX_PROCS 个槽位) 管理所有进程。
 * 这是最简单的实现, Linux 使用更复杂的链表/红黑树。
 *
 * 参考: Linux 内核 include/linux/sched.h struct task_struct
 * ================================================================== */
static struct proc proc_table[MAX_PROCS];

/* ==================================================================
 * 当前运行进程指针
 *
 * 始终指向当前 CPU 正在执行的进程。
 * 在单核系统中只需一个全局指针。
 * 参考: Linux 内核 current 宏 (per-CPU 变量)
 * ================================================================== */
static struct proc *current = NULL;

/* ==================================================================
 * current_proc() — 获取当前进程指针
 * ================================================================== */
struct proc *current_proc(void)
{
    return current;
}

/* ==================================================================
 * proc_init() — 初始化进程子系统
 *
 * 将所有 PCB 标记为未使用。
 * ================================================================== */
void proc_init(void)
{
    uart_puts("[proc] Initializing process subsystem...\n");

    /* 将所有 PCB 槽位标记为 UNUSED */
    for (int i = 0; i < MAX_PROCS; i++) {
        proc_table[i].state = PROC_UNUSED;
        proc_table[i].pid = i;
    }

    uart_puts("[proc] Process table initialized (");
    /* 打印最大进程数 */
    char buf[4];
    int n = MAX_PROCS;
    int i = 3;
    buf[i] = '\0';
    while (n > 0) {
        buf[--i] = '0' + (n % 10);
        n /= 10;
    }
    uart_puts(&buf[i]);
    uart_puts(" slots)\n");
}

/* ==================================================================
 * 用户态入口跳板函数
 *
 * proc_create 不能直接让新进程从 EL0 开始执行,
 * 因为进程切换发生在 EL1。需要一个内核态的"跳板"函数,
 * 由 switch_to 切换到此处后, 再通过 ERET 降级到 EL0。
 *
 * 流程:
 *   switch_to() → kthread_entry() → ret_to_user() → EL0 用户代码
 * ================================================================== */
static void kthread_entry(void)
{
    /* current 指向当前进程, 取出其用户态 PC 和 SP */
    struct proc *p = current;

    uart_puts("[proc] Entering user mode for PID ");
    uart_putc('0' + p->pid);
    uart_puts("\n");

    /* 调用汇编函数 ret_to_user, 通过 ERET 跳转到 EL0 */
    ret_to_user(p->user_pc, p->user_sp);
}

/* ==================================================================
 * proc_create() — 创建一个用户进程
 *
 * 步骤:
 *   1. 在进程表中找到空闲 PCB
 *   2. 分配内核栈 (1 个物理页面 = 4KB)
 *   3. 分配用户栈 (1 个物理页面 = 4KB)
 *   4. 设置 cpu_context, 使 switch_to 后跳转到 kthread_entry
 *   5. 记录用户态入口地址和栈指针
 *
 * 参数:
 *   entry — 用户态入口函数地址
 *
 * 返回:
 *   成功: 新进程的 PID
 *   失败: -1
 * ================================================================== */
int proc_create(void (*entry)(void))
{
    /* 步骤 1: 查找空闲 PCB 槽位 */
    struct proc *p = NULL;
    for (int i = 0; i < MAX_PROCS; i++) {
        if (proc_table[i].state == PROC_UNUSED) {
            p = &proc_table[i];
            break;
        }
    }
    if (p == NULL) {
        uart_puts("[proc] ERROR: No free PCB slot!\n");
        return -1;
    }

    /* 步骤 2: 分配内核栈 (4KB)
     * 每个进程需要自己的内核栈, 用于处理该进程的系统调用和中断。
     * 栈从高地址向低地址增长, 所以初始 SP = 栈基地址 + 页大小。 */
    uint64_t kstack = page_alloc();
    if (kstack == 0) {
        uart_puts("[proc] ERROR: Cannot allocate kernel stack!\n");
        return -1;
    }
    p->kstack = kstack;

    /* 步骤 3: 分配用户栈 (4KB)
     * 用户态程序使用的栈, 在 EL0 执行时使用 SP_EL0。 */
    uint64_t ustack = page_alloc();
    if (ustack == 0) {
        page_free(kstack);
        uart_puts("[proc] ERROR: Cannot allocate user stack!\n");
        return -1;
    }

    /* 步骤 4: 设置 CPU 上下文
     *
     * 当 switch_to 恢复此进程的 context 时:
     *   - LR (x30) = kthread_entry → ret 后跳转到 kthread_entry
     *   - SP = 内核栈顶
     *   - 其他 callee-saved 寄存器初始为 0
     *
     * 参考: [ARM-ABI] AAPCS64 "5.1.1 General-purpose Registers" */
    p->context.lr = (uint64_t)kthread_entry;  /* 函数返回后跳转到跳板函数 */
    p->context.sp = kstack + PAGE_SIZE;        /* 内核栈顶 (栈向下增长) */
    p->context.fp = 0;
    p->context.x19 = 0;
    p->context.x20 = 0;
    p->context.x21 = 0;
    p->context.x22 = 0;
    p->context.x23 = 0;
    p->context.x24 = 0;
    p->context.x25 = 0;
    p->context.x26 = 0;
    p->context.x27 = 0;
    p->context.x28 = 0;

    /* 步骤 5: 记录用户态信息
     * user_pc — 用户态代码入口 (ERET 时设为 ELR_EL1)
     * user_sp — 用户态栈顶 (ERET 时设为 SP_EL0) */
    p->user_pc = (uint64_t)entry;
    p->user_sp = ustack + PAGE_SIZE;           /* 用户栈顶 */

    /* 标记为就绪态 */
    p->state = PROC_READY;

    uart_puts("[proc] Created process PID=");
    uart_putc('0' + p->pid);
    uart_puts("\n");

    return p->pid;
}

/* ==================================================================
 * schedule() — 简单调度器
 *
 * 遍历进程表, 找到第一个 READY 进程并切换到它。
 * 这是最简单的调度算法 (非抢占式轮询)。
 *
 * 参考: [OSTEP] Chapter 7 "Scheduling: Introduction"
 * ================================================================== */
void schedule(void)
{
    struct proc *next = NULL;

    /* 遍历进程表寻找就绪进程 */
    for (int i = 0; i < MAX_PROCS; i++) {
        if (proc_table[i].state == PROC_READY) {
            next = &proc_table[i];
            break;
        }
    }

    if (next == NULL) {
        uart_puts("[sched] No ready process found.\n");
        return;
    }

    /* 标记新进程为运行态 */
    next->state = PROC_RUNNING;

    /* 保存旧的 current, 设置新的 current */
    struct proc *prev = current;
    current = next;

    if (prev == NULL) {
        /* 第一次调度: 没有前一个进程, 直接跳转到新进程
         * 创建一个虚拟的 context 用于接收 switch_to 的保存 */
        static struct cpu_context dummy;
        switch_to(&dummy, &next->context);
    } else {
        /* 正常上下文切换 */
        prev->state = PROC_READY;
        switch_to(&prev->context, &next->context);
    }
}
