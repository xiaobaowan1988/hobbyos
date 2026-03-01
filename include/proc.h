/*
 * include/proc.h — 进程管理头文件
 *
 * 定义进程控制块 (PCB) 和进程管理接口。
 *
 * 参考文档:
 *   [ARM-ARM] ARM Architecture Reference Manual ARMv8-A (DDI 0487)
 *   [ARM-ARM] D1.6 "Exception levels and execution states"
 */

#ifndef PROC_H
#define PROC_H

#include "types.h"

/* ==================================================================
 * 进程状态定义
 * ================================================================== */
#define PROC_UNUSED     0   /* 未使用的 PCB 槽位 */
#define PROC_READY      1   /* 就绪态: 可被调度运行 */
#define PROC_RUNNING    2   /* 运行态: 正在 CPU 上执行 */

/* ==================================================================
 * 最大进程数
 * ================================================================== */
#define MAX_PROCS       16

/* ==================================================================
 * CPU 上下文 (用于进程切换时保存/恢复)
 *
 * 只需保存 callee-saved 寄存器 (x19-x30, SP),
 * 因为进程切换发生在函数调用边界,
 * caller-saved 寄存器由编译器自动处理。
 *
 * 参考: [ARM-ABI] "AAPCS64: 5.1.1 General-purpose Registers"
 *        x19-x28: callee-saved
 *        x29: FP (frame pointer), callee-saved
 *        x30: LR (link register), callee-saved
 * ================================================================== */
struct cpu_context {
    uint64_t x19;       /* callee-saved 寄存器 */
    uint64_t x20;
    uint64_t x21;
    uint64_t x22;
    uint64_t x23;
    uint64_t x24;
    uint64_t x25;
    uint64_t x26;
    uint64_t x27;
    uint64_t x28;
    uint64_t fp;        /* x29: Frame Pointer */
    uint64_t lr;        /* x30: Link Register (函数返回地址) */
    uint64_t sp;        /* Stack Pointer */
};

/* ==================================================================
 * 进程控制块 (Process Control Block, PCB)
 *
 * 每个进程拥有一个 PCB, 保存进程的所有元数据。
 * 这是操作系统管理进程的核心数据结构。
 *
 * 参考: [OSTEP] Chapter 4 "The Abstraction: The Process"
 * ================================================================== */
struct proc {
    int pid;                    /* 进程 ID */
    int state;                  /* 进程状态 (PROC_UNUSED/READY/RUNNING) */
    struct cpu_context context; /* CPU 上下文 (用于进程切换) */
    uint64_t kstack;            /* 内核栈基地址 (每进程一个栈) */
    uint64_t user_sp;           /* 用户态栈指针 (EL0 SP) */
    uint64_t user_pc;           /* 用户态程序计数器 (EL0 入口) */
};

/* ==================================================================
 * 进程管理接口
 * ================================================================== */

/* proc_init() — 初始化进程子系统 */
void proc_init(void);

/* proc_create() — 创建一个用户进程
 *
 * 参数:
 *   entry — 用户态入口函数地址 (将在 EL0 执行)
 *
 * 返回:
 *   成功: 新进程的 PID
 *   失败: -1 */
int proc_create(void (*entry)(void));

/* switch_to() — 执行进程上下文切换 (汇编实现)
 *
 * 保存当前进程的 callee-saved 寄存器到 prev->context,
 * 从 next->context 恢复下一进程的寄存器。
 *
 * 参数:
 *   prev — 指向当前进程 context 的指针
 *   next — 指向下一进程 context 的指针 */
void switch_to(struct cpu_context *prev, struct cpu_context *next);

/* ret_to_user() — 从内核态跳转到用户态 (汇编实现)
 *
 * 设置 ELR_EL1 和 SPSR_EL1, 然后执行 ERET 跳转到 EL0。
 *
 * 参数:
 *   pc — 用户态入口地址
 *   sp — 用户态栈指针 */
void ret_to_user(uint64_t pc, uint64_t sp);

/* current_proc() — 获取当前运行的进程 PCB 指针 */
struct proc *current_proc(void);

#endif /* PROC_H */
