/*
 * user/user_prog.c — 用户态测试程序 (EL0)
 *
 * 演示完整的网络发送路径:
 *
 *   [ EL0 用户态 ]  "Hello World" (位于用户进程的虚拟内存中)
 *   |
 *   |-- 调用: send(socket_fd, "Hello World", 11, 0)
 *   |-- 触发: 执行 SVC #0 指令, 触发异常陷入内核
 *   v
 *   ============ 特权级边界 (硬件自动保存 PC/PSTATE → ELR_EL1/SPSR_EL1)
 *   |
 *   [ EL1 内核态 ]
 *   |-- syscall_handler() 识别 SYS_NET_SEND
 *   |-- 协议栈: 分配 sk_buff, 为 "Hello World" 加上 UDP/IP 头部
 *   |-- eth_send(): 添加以太网头
 *   |-- virtio_net_send(): 将帧放入 Tx Ring, 写 Doorbell 通知网卡
 *   v
 *   [ 网卡 (virtio-net) ] → DMA 读取内存 → 转换为网络信号发送
 *
 * 参考文档:
 *   [ARM-ARM] C6.2.24 "SVC, Supervisor Call"
 *   [LINUX-SYSCALL] Linux ARM64 syscall ABI
 */

#include "syscall.h"

/* ==================================================================
 * syscall_putc() — 用户态封装: 输出单个字符
 *
 * SVC #0 触发 Supervisor Call 异常:
 *   1. CPU 自动: PC → ELR_EL1, PSTATE → SPSR_EL1
 *   2. CPU 跳转到 VBAR_EL1 + 0x400 (低 EL 同步异常入口)
 *   3. vectors.S 保存 x0-x30, 调用 exception_handler()
 *   4. exception_handler 检测 EC=0x15(SVC), 调用 syscall_handler()
 *   5. syscall_handler 查表执行 sys_putc()
 *   6. ERET 返回用户态
 *
 * 参考: [ARM-ARM] C6.2.24 "SVC"
 * ================================================================== */
static void syscall_putc(char c)
{
    __asm__ volatile(
        "mov x0, %[ch]\n"          /* x0 = 字符参数 (第 1 参数) */
        "mov x8, %[nr]\n"          /* x8 = SYS_PUTC (系统调用号) */
        "svc #0\n"                 /* 触发 Supervisor Call → 陷入 EL1 */
        :
        : [ch] "r" ((unsigned long)c),
          [nr] "i" (SYS_PUTC)
        : "x0", "x8", "memory"
    );
}

/* ==================================================================
 * syscall_exit() — 用户态封装: 进程退出
 * ================================================================== */
static void syscall_exit(int status)
{
    __asm__ volatile(
        "mov x0, %[st]\n"
        "mov x8, %[nr]\n"
        "svc #0\n"
        :
        : [st] "r" ((unsigned long)status),
          [nr] "i" (SYS_EXIT)
        : "x0", "x8", "memory"
    );
}

/* ==================================================================
 * syscall_net_send() — 用户态封装: 通过网络发送数据
 *
 * 这对应用户流程图中的:
 *   send(socket_fd, "Hello World", 11, 0)
 *
 * 内核侧完整路径:
 *   SVC → syscall_handler → sys_net_send →
 *     udp_send():
 *       1. skb_alloc() — 分配 sk_buff
 *       2. skb_put()   — 放入 "Hello World" payload
 *       3. skb_push()  — 添加 UDP 头 (src_port, dst_port, len)
 *     ip_send():
 *       4. skb_push()  — 添加 IP 头 (src_ip, dst_ip, TTL, checksum)
 *       5. arp_lookup() — 解析目标 MAC (可能触发 ARP 请求)
 *     eth_send():
 *       6. skb_push()  — 添加以太网头 (dst_mac, src_mac, EtherType)
 *     virtio_net_send():
 *       7. 将帧数据复制到 DMA 缓冲区
 *       8. 设置 Tx Descriptor (地址 + 长度)
 *       9. 放入 Available Ring
 *      10. 写 QUEUE_NOTIFY (Doorbell) — 通知网卡有数据要发送!
 *     [网卡]:
 *      11. DMA 从统一内存读取帧数据
 *      12. 转换为电/光信号通过物理网络发送
 *
 * 参数:
 *   dst_ip   — 目标 IP (主机字节序, 如 IP4(10,0,2,1) = 0x0A000201)
 *   dst_port — 目标端口号
 *   buf      — 数据缓冲区
 *   len      — 数据长度
 *
 * 返回: 发送的字节数, 或 -1 错误
 *
 * 参考: [ARM-ARM] C6.2.24 "SVC"
 *        POSIX sendto() 语义
 * ================================================================== */
static long syscall_net_send(unsigned long dst_ip, unsigned long dst_port,
                             const void *buf, unsigned long len)
{
    long ret;
    __asm__ volatile(
        "mov x0, %[ip]\n"          /* x0 = 目标 IP 地址 */
        "mov x1, %[port]\n"        /* x1 = 目标端口 */
        "mov x2, %[buf]\n"         /* x2 = 数据缓冲区地址 */
        "mov x3, %[len]\n"         /* x3 = 数据长度 */
        "mov x8, %[nr]\n"          /* x8 = SYS_NET_SEND */
        "svc #0\n"                 /* 陷入内核 → 完整网络发送路径 */
        "mov %[ret], x0\n"         /* 返回值在 x0 */
        : [ret] "=r" (ret)
        : [ip]   "r" (dst_ip),
          [port] "r" (dst_port),
          [buf]  "r" ((unsigned long)buf),
          [len]  "r" (len),
          [nr]   "i" (SYS_NET_SEND)
        : "x0", "x1", "x2", "x3", "x8", "memory"
    );
    return ret;
}

/* ==================================================================
 * user_puts() — 用户态输出字符串 (通过 SVC)
 * ================================================================== */
static void user_puts(const char *s)
{
    while (*s) {
        if (*s == '\n')
            syscall_putc('\r');
        syscall_putc(*s);
        s++;
    }
}

/* ==================================================================
 * user_strlen() — 简单 strlen (用户态没有标准库)
 * ================================================================== */
static unsigned long user_strlen(const char *s)
{
    unsigned long len = 0;
    while (*s++) len++;
    return len;
}

/* ==================================================================
 * IP4() — 构造 IP 地址 (主机字节序)
 * ================================================================== */
#define IP4(a,b,c,d) (((unsigned long)(a)<<24)|((unsigned long)(b)<<16)|\
                      ((unsigned long)(c)<<8)|(unsigned long)(d))

/* ==================================================================
 * user_entry() — 用户态程序入口
 *
 * 此函数在 EL0 (用户态) 运行。
 * 演示完整的 "Hello World" 网络发送路径。
 * ================================================================== */
void user_entry(void)
{
    user_puts("==============================================\n");
    user_puts("  [EL0] User-space Network Send Demo\n");
    user_puts("==============================================\n\n");

    user_puts("[user] Running at EL0 (user mode, unprivileged)\n");
    user_puts("[user] All hardware access requires SVC syscalls\n\n");

    /* ---- 演示完整发送路径 ---- */
    user_puts("[user] Preparing to send 'Hello World' over the network...\n");
    user_puts("[user] Target: 10.0.2.1:9999 (QEMU gateway) via UDP\n\n");

    /* 这是用户流程图中的核心调用:
     *   send(socket_fd, "Hello World", 11, 0)
     *
     * 实际触发的硬件/软件路径:
     *   1. [EL0] SVC #0 → 触发异常, 硬件保存 PC/PSTATE
     *   2. [EL1] exception_handler → syscall_handler → sys_net_send
     *   3. [EL1] udp_send: skb_alloc + skb_put("Hello World") + skb_push(UDP头)
     *   4. [EL1] ip_send: skb_push(IP头) + ip_checksum + ARP解析
     *   5. [EL1] eth_send: skb_push(以太网头)
     *   6. [EL1] virtio_net_send: 写 Tx Descriptor + Available Ring
     *   7. [EL1] MMIO 写 QUEUE_NOTIFY (Doorbell) → 通知网卡
     *   8. [HW]  网卡 DMA 读取统一内存中的帧数据
     *   9. [HW]  网卡将数据转换为电信号发送到物理网络
     *  10. [EL1] ERET → 返回 EL0 用户态
     */
    const char *message = "Hello World";
    unsigned long msg_len = user_strlen(message);

    user_puts("[user] >>> Calling send() — SVC #0 triggers EL0→EL1 transition\n");
    user_puts("[user] >>> The kernel will: alloc sk_buff → add UDP/IP/ETH headers\n");
    user_puts("[user] >>>   → write to virtio Tx Ring → ring Doorbell → NIC sends!\n\n");

    long ret = syscall_net_send(
        IP4(10, 0, 2, 1),       /* 目标 IP: 10.0.2.1 (QEMU 网关) */
        9999,                    /* 目标端口: 9999 */
        message,                 /* 数据: "Hello World" */
        msg_len                  /* 长度: 11 字节 */
    );

    if (ret >= 0) {
        user_puts("[user] <<< send() returned successfully!\n");
        user_puts("[user] <<< The full path has been executed:\n");
        user_puts("[user]     EL0(SVC) → EL1(syscall) → UDP → IP → ETH → virtio → NIC\n\n");
    } else {
        user_puts("[user] <<< send() failed!\n");
    }

    /* 再发送第二条消息演示可重复性 */
    const char *msg2 = "ARM64 HobbyOS network stack works!";
    syscall_net_send(IP4(10, 0, 2, 1), 9999, msg2, user_strlen(msg2));
    user_puts("[user] Sent second message via UDP\n\n");

    user_puts("[user] Demo complete. Exiting.\n");
    syscall_exit(0);
}
