/*
 * net/tcp.c — TCP 层 (Layer 4) — 简化版
 *
 * TCP (Transmission Control Protocol) 是面向连接的可靠传输协议。
 * 实现 TCP 的完整状态机非常复杂, 本章仅实现最核心的子集:
 *   - 三次握手 (SYN → SYN-ACK → ACK)
 *   - 数据发送 (PSH + ACK)
 *   - 连接关闭 (FIN)
 *
 * 不实现: 重传、滑动窗口、拥塞控制、乱序重组等。
 *
 * 参考文档:
 *   [RFC793] Transmission Control Protocol
 */

#include "netdef.h"
#include "skbuff.h"
#include "net.h"
#include "string.h"
#include "uart.h"

/* ==================================================================
 * TCP 连接状态定义
 *
 * 参考: [RFC793] 3.2 "Terminology" — TCP 状态机
 * ================================================================== */
#define TCP_CLOSED      0
#define TCP_SYN_SENT    1   /* 已发送 SYN, 等待 SYN-ACK */
#define TCP_ESTABLISHED 2   /* 连接已建立 */
#define TCP_FIN_WAIT    3   /* 已发送 FIN, 等待 FIN-ACK */

/* ==================================================================
 * TCP 连接控制块 (TCB)
 *
 * 每个 TCP 连接的状态信息。
 * 真实 OS 使用哈希表管理大量连接, 我们简化为固定数组。
 *
 * 参考: [RFC793] 3.2 "Transmission Control Block"
 * ================================================================== */
#define MAX_TCP_CONNS   4

struct tcp_conn {
    int      state;         /* 连接状态 */
    uint32_t local_ip;      /* 本地 IP */
    uint16_t local_port;    /* 本地端口 */
    uint32_t remote_ip;     /* 远端 IP */
    uint16_t remote_port;   /* 远端端口 */
    uint32_t seq;           /* 发送序列号 (下一个要发送的字节号) */
    uint32_t ack;           /* 确认号 (下一个期望收到的字节号) */
};

static struct tcp_conn tcp_conns[MAX_TCP_CONNS];
static uint16_t tcp_port_counter = 49152;  /* 动态端口起始 */

/* ==================================================================
 * tcp_checksum() — 计算 TCP 校验和
 *
 * TCP 校验和包含伪头部 (Pseudo Header):
 *   | 源 IP (4) | 目标 IP (4) | 0 (1) | 协议=6 (1) | TCP长度 (2) |
 *   + TCP 头部 + 数据
 *
 * 参考: [RFC793] 3.1 "Header Format" — Checksum
 * ================================================================== */
static uint16_t tcp_checksum(uint32_t src_ip, uint32_t dst_ip,
                              const void *tcp_data, uint32_t tcp_len)
{
    uint32_t sum = 0;

    /* 伪头部: 源 IP */
    sum += (src_ip >> 16) & 0xFFFF;
    sum += src_ip & 0xFFFF;
    /* 伪头部: 目标 IP */
    sum += (dst_ip >> 16) & 0xFFFF;
    sum += dst_ip & 0xFFFF;
    /* 伪头部: 协议号 + TCP 长度 */
    sum += htons(IP_PROTO_TCP);
    sum += htons((uint16_t)tcp_len);

    /* TCP 数据 */
    const uint16_t *p = (const uint16_t *)tcp_data;
    uint32_t remaining = tcp_len;
    while (remaining > 1) {
        sum += *p++;
        remaining -= 2;
    }
    if (remaining == 1)
        sum += *(const uint8_t *)p;

    /* 进位回卷 */
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);

    return (uint16_t)(~sum);
}

/* ==================================================================
 * tcp_send_segment() — 发送一个 TCP 段
 *
 * 参数:
 *   conn    — TCP 连接控制块
 *   flags   — TCP 标志 (SYN, ACK, PSH, FIN)
 *   data    — 数据 (可为 NULL)
 *   datalen — 数据长度
 *
 * 参考: [RFC793] 3.1 "TCP Segment Format"
 * ================================================================== */
static void tcp_send_segment(struct tcp_conn *conn, uint8_t flags,
                             const void *data, uint32_t datalen)
{
    struct sk_buff *skb = skb_alloc();
    if (!skb) return;

    /* 放入数据 payload (如果有) */
    if (data && datalen > 0) {
        uint8_t *p = skb_put(skb, datalen);
        memcpy(p, data, datalen);
    }

    /* 添加 TCP 头 (20 字节, 无选项)
     *
     * TCP 头格式:
     *   | 源端口 (2) | 目标端口 (2) |
     *   | 序列号 (4) |
     *   | 确认号 (4) |
     *   | 数据偏移(4bit)+保留(4bit) | 标志 (1) | 窗口 (2) |
     *   | 校验和 (2) | 紧急指针 (2) |
     *
     * 参考: [RFC793] 3.1 "Header Format" */
    struct tcphdr *tcp = (struct tcphdr *)skb_push(skb, sizeof(struct tcphdr));

    tcp->src_port = htons(conn->local_port);
    tcp->dst_port = htons(conn->remote_port);
    tcp->seq      = htonl(conn->seq);
    tcp->ack_seq  = htonl(conn->ack);

    /* data_off: 数据偏移 = 5 (×4 = 20 字节, 无选项)
     * 高 4 位是数据偏移, 低 4 位保留
     * 参考: [RFC793] "Data Offset" */
    tcp->data_off = (5 << 4);
    tcp->flags    = flags;

    /* 窗口大小: 通告对端我们能接收的数据量
     * 简化: 使用固定值 8192
     * 参考: [RFC793] "Window" */
    tcp->window   = htons(8192);
    tcp->urgent   = 0;

    /* 计算校验和
     * 参考: [RFC793] "Checksum" */
    tcp->checksum = 0;
    tcp->checksum = tcp_checksum(
        htonl(conn->local_ip), htonl(conn->remote_ip),
        tcp, sizeof(struct tcphdr) + datalen);

    /* 更新序列号
     * SYN 和 FIN 各消耗 1 个序列号
     * 数据消耗 datalen 个序列号
     * 参考: [RFC793] 3.3 "Sequence Numbers" */
    if (flags & TCP_SYN) conn->seq++;
    if (flags & TCP_FIN) conn->seq++;
    conn->seq += datalen;

    /* 交给 IP 层发送 */
    ip_send(skb, conn->remote_ip, IP_PROTO_TCP);

    skb_free(skb);
}

/* ==================================================================
 * tcp_connect() — 发起 TCP 连接 (三次握手的第一步)
 *
 * 步骤:
 *   1. 分配连接控制块
 *   2. 发送 SYN 段
 *   3. 等待 SYN-ACK
 *   4. 发送 ACK — 连接建立
 *
 * 参数:
 *   dst_ip   — 目标 IP (主机字节序)
 *   dst_port — 目标端口
 *
 * 返回: socket 句柄 (连接索引), 或 -1 失败
 *
 * 参考: [RFC793] 3.4 "Establishing a connection" — 三次握手
 *   Client           Server
 *     |--- SYN -------->|    (1. 客户端发送 SYN)
 *     |<-- SYN-ACK -----|    (2. 服务器回复 SYN-ACK)
 *     |--- ACK -------->|    (3. 客户端发送 ACK, 连接建立)
 * ================================================================== */
int tcp_connect(uint32_t dst_ip, uint16_t dst_port)
{
    /* 查找空闲连接槽 */
    int idx = -1;
    for (int i = 0; i < MAX_TCP_CONNS; i++) {
        if (tcp_conns[i].state == TCP_CLOSED) {
            idx = i;
            break;
        }
    }
    if (idx == -1) {
        uart_puts("[tcp] No free connection slots!\n");
        return -1;
    }

    struct tcp_conn *conn = &tcp_conns[idx];
    conn->local_ip    = eth_get_ip();
    conn->local_port  = tcp_port_counter++;
    conn->remote_ip   = dst_ip;
    conn->remote_port = dst_port;
    conn->seq         = 1000;           /* 初始序列号 (简化, 应随机) */
    conn->ack         = 0;
    conn->state       = TCP_SYN_SENT;

    /* 发送 SYN (三次握手第一步)
     * 参考: [RFC793] "OPEN Call — Active OPEN" */
    uart_puts("[tcp] Sending SYN to establish connection...\n");
    tcp_send_segment(conn, TCP_SYN, NULL, 0);

    /* 简化: 忙等待 SYN-ACK (真实 OS 使用异步状态机)
     * 最多等待 ~100 次轮询 */
    for (int i = 0; i < 100; i++) {
        /* 检查是否收到了 SYN-ACK (tcp_recv 会更新 state) */
        if (conn->state == TCP_ESTABLISHED) {
            uart_puts("[tcp] Connection established!\n");
            return idx;
        }
        /* 短暂延时 (没有真正的 sleep, 用空循环代替) */
        for (volatile int j = 0; j < 100000; j++) ;
    }

    uart_puts("[tcp] Connection timeout!\n");
    conn->state = TCP_CLOSED;
    return -1;
}

/* ==================================================================
 * tcp_send() — 通过已建立的 TCP 连接发送数据
 *
 * 参数:
 *   sock — 连接句柄 (tcp_connect 的返回值)
 *   data — 要发送的数据
 *   len  — 数据长度
 *
 * 返回: 发送的字节数, 或 -1 错误
 *
 * 参考: [RFC793] "SEND Call"
 * ================================================================== */
int tcp_send(int sock, const void *data, uint32_t len)
{
    if (sock < 0 || sock >= MAX_TCP_CONNS) return -1;
    struct tcp_conn *conn = &tcp_conns[sock];
    if (conn->state != TCP_ESTABLISHED) return -1;

    /* 发送数据段 (PSH + ACK)
     * PSH: 告知接收方立即将数据交给应用层
     * ACK: 同时确认之前收到的数据
     * 参考: [RFC793] "Segment Send" */
    tcp_send_segment(conn, TCP_PSH | TCP_ACK, data, len);

    uart_puts("[tcp] Sent ");
    char buf[6];
    int i = 5;
    buf[i] = '\0';
    uint32_t v = len;
    if (v == 0) buf[--i] = '0';
    while (v > 0) { buf[--i] = '0' + (v % 10); v /= 10; }
    uart_puts(&buf[i]);
    uart_puts(" bytes\n");

    return (int)len;
}

/* ==================================================================
 * tcp_recv() — 处理收到的 TCP 段
 *
 * 由 IP 层在识别到 protocol=6(TCP) 时调用。
 * 根据 TCP 状态机处理不同类型的段。
 *
 * 参考: [RFC793] 3.9 "Event Processing"
 * ================================================================== */
void tcp_recv(uint32_t src_ip, const uint8_t *data, uint32_t len)
{
    if (len < sizeof(struct tcphdr)) return;

    const struct tcphdr *tcp = (const struct tcphdr *)data;
    uint16_t src_port = ntohs(tcp->src_port);
    uint16_t dst_port = ntohs(tcp->dst_port);

    /* 查找匹配的连接 */
    struct tcp_conn *conn = NULL;
    for (int i = 0; i < MAX_TCP_CONNS; i++) {
        if (tcp_conns[i].state != TCP_CLOSED &&
            tcp_conns[i].remote_ip == src_ip &&
            tcp_conns[i].remote_port == src_port &&
            tcp_conns[i].local_port == dst_port) {
            conn = &tcp_conns[i];
            break;
        }
    }
    if (!conn) return;

    uint32_t remote_seq = ntohl(tcp->seq);
    uint32_t remote_ack = ntohl(tcp->ack_seq);
    uint8_t  flags = tcp->flags;

    (void)remote_ack;

    /* 根据当前状态处理
     * 参考: [RFC793] 3.9 "SEGMENT ARRIVES" */
    switch (conn->state) {
    case TCP_SYN_SENT:
        /* 期望收到 SYN-ACK
         * 参考: [RFC793] "SYN-SENT STATE" */
        if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) {
            conn->ack = remote_seq + 1;  /* 确认 SYN (SYN 消耗 1 个序列号) */

            /* 发送 ACK (三次握手第三步)
             * 参考: [RFC793] "third segment of three-way handshake" */
            conn->state = TCP_ESTABLISHED;
            tcp_send_segment(conn, TCP_ACK, NULL, 0);
        }
        break;

    case TCP_ESTABLISHED:
        /* 更新确认号 */
        {
            uint8_t data_off = (tcp->data_off >> 4) * 4;
            uint32_t payload_len = len - data_off;
            if (payload_len > 0) {
                conn->ack = remote_seq + payload_len;
                /* 发送 ACK 确认收到的数据 */
                tcp_send_segment(conn, TCP_ACK, NULL, 0);
            }
        }
        break;

    case TCP_FIN_WAIT:
        /* 收到 FIN-ACK */
        if (flags & TCP_FIN) {
            conn->ack = remote_seq + 1;
            tcp_send_segment(conn, TCP_ACK, NULL, 0);
            conn->state = TCP_CLOSED;
        }
        break;
    }
}

/* ==================================================================
 * tcp_close() — 关闭 TCP 连接
 *
 * 发送 FIN 段通知对端关闭连接。
 *
 * 参考: [RFC793] 3.5 "Closing a Connection"
 * ================================================================== */
void tcp_close(int sock)
{
    if (sock < 0 || sock >= MAX_TCP_CONNS) return;
    struct tcp_conn *conn = &tcp_conns[sock];
    if (conn->state != TCP_ESTABLISHED) return;

    /* 发送 FIN + ACK
     * 参考: [RFC793] "CLOSE Call" */
    conn->state = TCP_FIN_WAIT;
    tcp_send_segment(conn, TCP_FIN | TCP_ACK, NULL, 0);
    uart_puts("[tcp] Connection closing (FIN sent)\n");

    /* 简化: 直接标记为关闭 */
    conn->state = TCP_CLOSED;
}
