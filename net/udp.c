/*
 * net/udp.c — UDP 层 (Layer 4)
 *
 * UDP (User Datagram Protocol) 是一种无连接、不可靠的传输层协议。
 * 相比 TCP, UDP 更简单: 没有握手、没有重传、没有流控。
 * 适用于 DNS、DHCP、实时音视频等场景。
 *
 * 参考文档:
 *   [RFC768] User Datagram Protocol
 */

#include "netdef.h"
#include "skbuff.h"
#include "net.h"
#include "string.h"
#include "uart.h"

/* ==================================================================
 * udp_send() — 发送 UDP 数据包
 *
 * 步骤:
 *   1. 分配 sk_buff, 放入 payload
 *   2. 添加 UDP 头
 *   3. 交给 IP 层发送
 *
 * 参数:
 *   dst_ip   — 目标 IP (主机字节序)
 *   src_port — 源端口
 *   dst_port — 目标端口
 *   data     — 用户数据
 *   len      — 数据长度
 *
 * 参考: [RFC768] "Format"
 * ================================================================== */
void udp_send(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
              const void *data, uint32_t len)
{
    /* 步骤 1: 分配 sk_buff */
    struct sk_buff *skb = skb_alloc();
    if (!skb) {
        uart_puts("[udp] skb_alloc failed!\n");
        return;
    }

    /* 步骤 2: 在 tail 放入 payload (应用数据)
     * skb_put 移动 tail, 返回写入位置。
     * 参考: [ULP] "Copying data into sk_buff" */
    uint8_t *payload = skb_put(skb, len);
    memcpy(payload, data, len);

    /* 步骤 3: 在 head 添加 UDP 头 (8 字节)
     * skb_push 将 data 前移, 腾出 UDP 头空间。
     *
     * UDP 头格式 (8 字节):
     *   | 源端口 (2) | 目标端口 (2) | 长度 (2) | 校验和 (2) |
     *
     * 参考: [RFC768] "User Datagram Header Format" */
    struct udphdr *udp = (struct udphdr *)skb_push(skb, sizeof(struct udphdr));

    udp->src_port = htons(src_port);     /* 源端口 (网络字节序) */
    udp->dst_port = htons(dst_port);     /* 目标端口 */
    udp->len      = htons(sizeof(struct udphdr) + len);  /* UDP总长度 */
    udp->checksum = 0;                   /* 校验和 = 0 (UDP 校验和可选) */

    /* 步骤 4: 交给 IP 层
     * IP 层会添加 IP 头, 然后交给以太网层发送。 */
    ip_send(skb, dst_ip, IP_PROTO_UDP);

    /* 步骤 5: 释放 sk_buff */
    skb_free(skb);
}

/* ==================================================================
 * udp_recv() — 处理收到的 UDP 数据包
 *
 * 由 IP 层在识别到 protocol=17(UDP) 时调用。
 *
 * 参数:
 *   src_ip — 源 IP 地址 (主机字节序)
 *   data   — UDP 数据 (含 UDP 头)
 *   len    — 数据长度
 *
 * 参考: [RFC768] "Receiving"
 * ================================================================== */
void udp_recv(uint32_t src_ip, const uint8_t *data, uint32_t len)
{
    if (len < sizeof(struct udphdr)) return;

    const struct udphdr *udp = (const struct udphdr *)data;
    uint16_t src_port = ntohs(udp->src_port);
    uint16_t dst_port = ntohs(udp->dst_port);
    uint16_t udp_len  = ntohs(udp->len);

    (void)src_ip;
    (void)src_port;
    (void)dst_port;
    (void)udp_len;

    /* 当前仅打印接收信息 (后续可实现 socket 分发) */
    uart_puts("[udp] Received packet\n");
}
