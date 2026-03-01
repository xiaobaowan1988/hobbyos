/*
 * kernel/syscall.c — 系统调用处理
 *
 * 当用户态程序执行 SVC #0 指令时, CPU 触发同步异常并陷入 EL1。
 * 异常处理流程: vectors.S → exception_handler() → syscall_handler()
 *
 * 系统调用遵循 Linux ARM64 的寄存器约定:
 *   x8      — 系统调用号 (syscall number)
 *   x0-x5   — 参数 (最多 6 个参数)
 *   x0      — 返回值 (放在 frame 中, ERET 后用户态从 x0 获取)
 *
 * 参考文档:
 *   [ARM-ARM] C6.2.24 "SVC, Supervisor Call"
 *   [LINUX-SYSCALL] Linux arch/arm64/kernel/syscall.c
 *   [ARM-ARM] D13.2.36 "ESR_EL1" — EC=0x15 表示 SVC from AArch64
 */

#include "syscall.h"
#include "uart.h"
#include "proc.h"
#include "net.h"
#include "netdef.h"

/* ==================================================================
 * sys_putc() — 系统调用: 输出单个字符
 *
 * 用户态程序无法直接访问 UART MMIO 寄存器 (属于 EL1 设备内存),
 * 必须通过系统调用请求内核代为输出。
 *
 * 参数:
 *   x0 — 要输出的字符 (低 8 位)
 *
 * 返回: 0 (成功)
 *
 * 类似: Linux 的 write(1, &c, 1)
 * ================================================================== */
static long sys_putc(unsigned long c)
{
    uart_putc((char)c);
    return 0;
}

/* ==================================================================
 * sys_exit() — 系统调用: 进程退出
 *
 * 当前实现: 打印退出信息后进入死循环。
 * 后续章节可实现进程回收和重新调度。
 *
 * 参数:
 *   x0 — 退出码 (当前仅打印)
 *
 * 类似: Linux 的 exit(status)
 * ================================================================== */
static long sys_exit(unsigned long status)
{
    uart_puts("[syscall] Process exited with status ");
    uart_putc('0' + (status & 0xF));
    uart_puts("\n");

    /* 简单实现: 进入死循环 (后续改为调度下一进程) */
    while (1)
        __asm__ volatile("wfe");

    return 0;  /* 不可达 */
}

/* ==================================================================
 * sys_write() — 系统调用: 写数据到输出
 *
 * 简化版: 忽略 fd, 直接将 buf 中的数据写到 UART。
 *
 * 参数:
 *   x0 — fd (文件描述符, 当前忽略)
 *   x1 — buf (用户态缓冲区地址)
 *   x2 — len (字节数)
 *
 * 返回: 实际写入的字节数
 *
 * 类似: Linux 的 write(fd, buf, count)
 * 注意: 由于使用恒等映射, 用户态地址 == 物理地址,
 *       内核可以直接访问用户态缓冲区。
 *       真实 OS 需要 copy_from_user() 检查地址合法性。
 * ================================================================== */
static long sys_write(unsigned long fd, unsigned long buf, unsigned long len)
{
    (void)fd;   /* 当前忽略 fd */
    const char *p = (const char *)buf;
    for (unsigned long i = 0; i < len; i++) {
        uart_putc(p[i]);
    }
    return (long)len;
}

/* ==================================================================
 * sys_net_send() — 系统调用: 通过 UDP 发送数据
 *
 * 这是用户态 send() 的内核实现。
 * 完整路径: 用户态 SVC → 内核 syscall → UDP → IP → Ethernet → virtio-net → NIC
 *
 * 参数:
 *   x0 — dst_ip (目标 IP, 主机字节序)
 *   x1 — dst_port (目标端口)
 *   x2 — buf (用户态数据缓冲区地址)
 *   x3 — len (数据长度)
 *
 * 返回: 发送的字节数, 或 -1 错误
 *
 * 这就是用户流程图中:
 *   EL0: send(socket_fd, "Hello World", 11, 0)
 *     → SVC #0 → EL1
 *     → 协议栈: 分配 sk_buff, 加 TCP/IP 头
 *     → DMA 映射 → 写 Tx Ring → Doorbell → 网卡发送
 * ================================================================== */
static long sys_net_send(unsigned long dst_ip, unsigned long dst_port,
                         unsigned long buf, unsigned long len)
{
    uart_puts("[syscall] sys_net_send: sending via UDP...\n");

    /* 使用固定源端口 12345, 目标端口由用户指定 */
    udp_send((uint32_t)dst_ip, 12345, (uint16_t)dst_port,
             (const void *)buf, (uint32_t)len);

    return (long)len;
}

/* ==================================================================
 * sys_sock_send() — 系统调用: 通过 TCP socket 发送数据
 *
 * 参数:
 *   x0 — socket 句柄 (由 tcp_connect 返回)
 *   x1 — buf (数据缓冲区地址)
 *   x2 — len (数据长度)
 *
 * 返回: 发送的字节数, 或 -1 错误
 * ================================================================== */
static long sys_sock_send(unsigned long sock, unsigned long buf,
                          unsigned long len)
{
    return (long)tcp_send((int)sock, (const void *)buf, (uint32_t)len);
}

/* ==================================================================
 * 系统调用表 — 函数指针数组
 *
 * 以系统调用号为索引, 查找对应的处理函数。
 * 每个条目是一个函数指针, 接受最多 6 个 unsigned long 参数。
 *
 * 参考: Linux 内核 arch/arm64/kernel/sys.c sys_call_table[]
 * ================================================================== */
typedef long (*syscall_fn_t)(unsigned long, unsigned long,
                             unsigned long, unsigned long,
                             unsigned long, unsigned long);

static syscall_fn_t syscall_table[NR_SYSCALLS] = {
    [SYS_PUTC]      = (syscall_fn_t)sys_putc,       /* 0: 输出字符 */
    [SYS_EXIT]      = (syscall_fn_t)sys_exit,        /* 1: 进程退出 */
    [SYS_WRITE]     = (syscall_fn_t)sys_write,       /* 2: 写数据 */
    [SYS_NET_SEND]  = (syscall_fn_t)sys_net_send,    /* 3: UDP 发送 */
    [SYS_SOCK_SEND] = (syscall_fn_t)sys_sock_send,   /* 4: TCP 发送 */
};

/* ==================================================================
 * syscall_handler() — 系统调用分发器
 *
 * 从寄存器帧中提取系统调用号和参数,
 * 查表调用对应的处理函数, 并将返回值写回 x0。
 *
 * 参数:
 *   frame — 异常向量保存的寄存器帧
 *           frame[0]  = x0  (arg0)
 *           frame[1]  = x1  (arg1)
 *           frame[2]  = x2  (arg2)
 *           frame[3]  = x3  (arg3)
 *           frame[4]  = x4  (arg4)
 *           frame[5]  = x5  (arg5)
 *           frame[8]  = x8  (syscall number)
 *           frame[32] = ELR_EL1
 *
 * 参考: [LINUX-SYSCALL] el0_svc_handler()
 * ================================================================== */
void syscall_handler(unsigned long *frame)
{
    /* 从 frame 中提取系统调用号 (x8) 和参数 (x0-x5)
     *
     * ARM64 系统调用约定:
     *   x8 = 系统调用号
     *   x0-x5 = 参数
     *   返回值放在 x0
     *
     * 参考: [LINUX-SYSCALL] "AArch64 syscall ABI" */
    unsigned long sysno = frame[8];    /* x8 = 系统调用号 */
    unsigned long arg0  = frame[0];    /* x0 */
    unsigned long arg1  = frame[1];    /* x1 */
    unsigned long arg2  = frame[2];    /* x2 */
    unsigned long arg3  = frame[3];    /* x3 */
    unsigned long arg4  = frame[4];    /* x4 */
    unsigned long arg5  = frame[5];    /* x5 */

    /* 检查系统调用号是否有效 */
    if (sysno >= NR_SYSCALLS || syscall_table[sysno] == NULL) {
        uart_puts("[syscall] Unknown syscall: ");
        uart_putc('0' + (sysno & 0xF));
        uart_puts("\n");
        frame[0] = (unsigned long)-1;   /* 返回 -1 表示错误 */
        return;
    }

    /* 调用对应的系统调用处理函数 */
    long ret = syscall_table[sysno](arg0, arg1, arg2, arg3, arg4, arg5);

    /* 将返回值写入 frame[0] (即 x0)
     * ERET 恢复寄存器后, 用户态将从 x0 获得此返回值 */
    frame[0] = (unsigned long)ret;
}
