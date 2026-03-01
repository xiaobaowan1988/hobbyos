/*
 * include/syscall.h — 系统调用号定义与接口
 *
 * 定义系统调用号 (syscall numbers) 和内核侧的系统调用处理函数。
 * 用户态程序通过 SVC #0 指令触发系统调用,
 * 系统调用号通过 x8 寄存器传递 (遵循 Linux ARM64 惯例)。
 *
 * 参考文档:
 *   [ARM-ARM] ARM Architecture Reference Manual ARMv8-A (DDI 0487)
 *   [LINUX-SYSCALL] Linux 内核 arch/arm64/include/asm/unistd.h
 *   [ARM-ARM] C6.2.24 "SVC, Supervisor Call"
 */

#ifndef SYSCALL_H
#define SYSCALL_H

#include "types.h"

/* ==================================================================
 * 系统调用号定义
 *
 * 编号从 0 开始, 与 Linux 的 ARM64 系统调用号不同
 * (Linux 使用不同的编号方案)。
 * 后续章节会逐步添加更多系统调用。
 * ================================================================== */
#define SYS_PUTC      0   /* 输出一个字符到串口 (调试用) */
#define SYS_EXIT      1   /* 进程退出 */
#define SYS_WRITE     2   /* 写数据到文件描述符 */
#define SYS_NET_SEND  3   /* 通过网络发送数据 (UDP) */
#define SYS_SOCK_SEND 4   /* 通过 TCP socket 发送数据 */

#define NR_SYSCALLS 16  /* 系统调用表大小 */

/* ==================================================================
 * syscall_handler() — 系统调用分发器 (从异常处理中调用)
 *
 * 参数:
 *   frame — 指向保存的寄存器帧 (来自异常向量的 SAVE_REGS)
 *           frame[0]  = x0 (第1个参数)
 *           frame[1]  = x1 (第2个参数)
 *           frame[2]  = x2 (第3个参数)
 *           ...
 *           frame[8]  = x8 (系统调用号)
 *           frame[32] = ELR_EL1 (返回地址)
 *           frame[33] = SPSR_EL1
 * ================================================================== */
void syscall_handler(unsigned long *frame);

#endif /* SYSCALL_H */
