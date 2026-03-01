/*
 * net/ip.c — IPv4 层 (Layer 3)
 *
 * IP 层负责:
 *   发送: 添加 IP 头 (源/目标 IP, TTL, 协议号, 校验和), 然后交给以太网层
 *   接收: 验证 IP 头, 根据协议号分发到 TCP/UDP/ICMP
 *
 * 当前实现:
 *   - 仅支持 IPv4
 *   - 不支持分片/重组 (假设所有包 <= MTU)
 *   - 不支持 IP 选项
 *
 * 参考文档:
 *   [RFC791] Internet Protocol — DARPA Internet Program Protocol Specification
 */

#include "netdef.h"
#include "skbuff.h"
#include "net.h"
#include "string.h"
#include "uart.h"

/* ==================================================================
 * IP 标识符计数器 (用于分片, 每发一个包递增)
 * 参考: [RFC791] "Identification" field
 * ================================================================== */
static uint16_t ip_id_counter = 0;

/* ==================================================================
 * ip_checksum() — 计算 IP 头部校验和
 *
 * IP 校验和算法:
 *   1. 将头部视为 16-bit 字的序列
 *   2. 逐字累加 (进位回卷, 即 ones' complement addition)
 *   3. 取反得到校验和
 *
 * 参数:
 *   data — IP 头部数据
 *   len  — 头部长度 (字节, 通常 20)
 *
 * 返回: 16-bit 校验和 (网络字节序)
 *
 * 参考: [RFC791] "Header Checksum" — ones' complement of the
 *        ones' complement sum of all 16-bit words in the header
 * ================================================================== */
static uint16_t ip_checksum(const void *data, uint32_t len)
{
    const uint16_t *p = (const uint16_t *)data;
    uint32_t sum = 0;

    /* 累加所有 16-bit 字 */
    while (len > 1) {
        sum += *p++;
        len -= 2;
    }
    /* 如果有奇数字节, 补零后加上 */
    if (len == 1) {
        sum += *(const uint8_t *)p;
    }
    /* 进位回卷: 将高 16 位加到低 16 位 */
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    /* 取反 */
    return (uint16_t)(~sum);
}

/* ==================================================================
 * ip_send() — 发送 IP 数据包
 *
 * 步骤:
 *   1. 在 sk_buff 头部添加 IP 头 (20 字节)
 *   2. 填入各字段 (版本、TTL、协议号、源/目标 IP 等)
 *   3. 计算头部校验和
 *   4. ARP 解析目标 MAC 地址
 *   5. 交给以太网层发送
 *
 * 参数:
 *   skb      — 包含上层数据 (TCP/UDP payload) 的 sk_buff
 *   dst_ip   — 目标 IP 地址 (主机字节序)
 *   protocol — 上层协议号 (IP_PROTO_TCP/UDP/ICMP)
 *
 * 参考: [RFC791] "Internet Header Format"
 * ================================================================== */
void ip_send(struct sk_buff *skb, uint32_t dst_ip, uint8_t protocol)
{
    /* 步骤 1: 在 sk_buff 头部添加 IP 头
     *
     * skb_push 将 data 前移 20 字节, 返回新 data 位置。
     * 此时 skb 的数据布局:
     *   [IP头 (20B)] [TCP/UDP头 + payload]
     *
     * 参考: [ULP] "Adding IP Header" */
    struct iphdr *ip = (struct iphdr *)skb_push(skb, sizeof(struct iphdr));

    /* 步骤 2: 填入 IP 头各字段 */

    /* ver_ihl: 版本 (4) + 头部长度 (5 × 4 = 20 字节, 无选项)
     * 参考: [RFC791] "Version" = 4, "IHL" = 5 */
    ip->ver_ihl = (4 << 4) | 5;

    /* TOS: 服务类型 = 0 (默认)
     * 参考: [RFC791] "Type of Service" */
    ip->tos = 0;

    /* 总长度: IP 头 + 上层数据 (网络字节序)
     * 参考: [RFC791] "Total Length" */
    ip->tot_len = htons(skb->len);

    /* 标识符: 每包递增 (用于分片重组)
     * 参考: [RFC791] "Identification" */
    ip->id = htons(ip_id_counter++);

    /* 分片偏移: 0x4000 = Don't Fragment 标志
     * 参考: [RFC791] "Fragment Offset" + "Flags" */
    ip->frag_off = htons(0x4000);

    /* TTL: 生存时间 = 64 跳 (Linux 默认值)
     * 每经过一个路由器 TTL 减 1, 到 0 时丢弃。
     * 参考: [RFC791] "Time to Live" */
    ip->ttl = 64;

    /* 协议号: 标识上层协议
     * 参考: [RFC791] "Protocol" (6=TCP, 17=UDP) */
    ip->protocol = protocol;

    /* 源 IP 地址: 本机 IP (网络字节序)
     * 参考: [RFC791] "Source Address" */
    ip->src_ip = htonl(eth_get_ip());

    /* 目标 IP 地址 (网络字节序)
     * 参考: [RFC791] "Destination Address" */
    ip->dst_ip = htonl(dst_ip);

    /* 步骤 3: 计算 IP 头部校验和
     *
     * 校验和字段先置零, 然后对整个头部计算 ones' complement sum。
     * 参考: [RFC791] "Header Checksum" */
    ip->checksum = 0;
    ip->checksum = ip_checksum(ip, sizeof(struct iphdr));

    /* 步骤 4: ARP 解析目标 MAC 地址
     *
     * 需要知道下一跳 (next hop) 的 MAC 地址:
     *   - 如果目标 IP 在同一子网: 直接解析目标 IP 的 MAC
     *   - 如果在不同子网: 解析网关 (gateway) 的 MAC
     *
     * 参考: [RFC826] ARP 使用场景 */
    uint32_t next_hop;
    uint32_t my_ip_val = eth_get_ip();
    if ((dst_ip & (SUBNET_MASK >> 0)) == (my_ip_val & (SUBNET_MASK >> 0))) {
        next_hop = dst_ip;          /* 同子网, 直接发 */
    } else {
        next_hop = GATEWAY_IP;       /* 不同子网, 发往网关 */
    }

    uint8_t dst_mac[6];
    if (arp_lookup(next_hop, dst_mac) != 0) {
        /* ARP 缓存未命中, 发送 ARP 请求
         * 简化处理: 发送 ARP 请求后使用广播地址
         * (真实 OS 会将包排队等待 ARP Reply) */
        arp_send_request(next_hop);
        /* 使用广播地址临时发送 (不完美但可工作) */
        memset(dst_mac, 0xFF, 6);
    }

    /* 步骤 5: 交给以太网层发送
     * eth_send 会在 sk_buff 前面再加上以太网头。 */
    eth_send(skb, dst_mac, ETH_P_IP);
}

/* ==================================================================
 * ip_recv() — 处理收到的 IP 数据包
 *
 * 步骤:
 *   1. 验证 IP 头 (版本、长度、校验和)
 *   2. 检查目标 IP 是否为本机
 *   3. 根据协议号分发到 TCP/UDP
 *
 * 参数:
 *   data — IP 数据包 (含 IP 头)
 *   len  — 数据长度
 *
 * 参考: [RFC791] "Internet Module Processing"
 * ================================================================== */
void ip_recv(const uint8_t *data, uint32_t len)
{
    if (len < sizeof(struct iphdr)) return;

    const struct iphdr *ip = (const struct iphdr *)data;

    /* 检查版本 = 4 */
    uint8_t version = (ip->ver_ihl >> 4) & 0xF;
    if (version != 4) return;

    /* 获取 IP 头长度 */
    uint8_t ihl = (ip->ver_ihl & 0xF) * 4;
    if (ihl < 20 || ihl > len) return;

    /* 验证校验和 */
    if (ip_checksum(ip, ihl) != 0) {
        uart_puts("[ip] Checksum error, dropping packet\n");
        return;
    }

    /* 检查目标 IP */
    uint32_t dst = ntohl(ip->dst_ip);
    if (dst != eth_get_ip() && dst != 0xFFFFFFFF) {
        return;  /* 不是发给我们的 */
    }

    /* 提取上层数据 */
    const uint8_t *payload = data + ihl;
    uint32_t payload_len = ntohs(ip->tot_len) - ihl;
    uint32_t src_ip = ntohl(ip->src_ip);

    /* 根据协议号分发 */
    switch (ip->protocol) {
    case IP_PROTO_TCP:
        tcp_recv(src_ip, payload, payload_len);
        break;
    case IP_PROTO_UDP:
        udp_recv(src_ip, payload, payload_len);
        break;
    default:
        break;
    }
}
