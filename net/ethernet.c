/*
 * net/ethernet.c — 以太网层 (Layer 2) 和 ARP 协议
 *
 * 以太网层负责:
 *   - 发送: 为 sk_buff 添加以太网头 (src MAC, dst MAC, EtherType)
 *   - 接收: 剥离以太网头, 根据 EtherType 分发到上层 (IP/ARP)
 *
 * ARP 协议负责:
 *   - 将 IP 地址解析为 MAC 地址 (需要时发送 ARP Request)
 *   - 维护 ARP 缓存表
 *   - 处理收到的 ARP Request/Reply
 *
 * 参考文档:
 *   [IEEE802.3] IEEE 802.3 Ethernet Standard
 *   [RFC826]  ARP — An Ethernet Address Resolution Protocol
 */

#include "netdef.h"
#include "skbuff.h"
#include "virtio.h"
#include "string.h"
#include "uart.h"

/* ==================================================================
 * ARP 缓存表
 *
 * 简单的静态数组, 存储已解析的 IP → MAC 映射。
 * 真实 OS 使用哈希表和超时机制。
 *
 * 参考: Linux 内核 net/ipv4/arp.c arp_tbl
 * ================================================================== */
#define ARP_CACHE_SIZE  16

struct arp_entry {
    uint32_t ip;                /* IP 地址 (主机字节序) */
    uint8_t  mac[6];            /* 对应的 MAC 地址 */
    uint8_t  valid;             /* 1 = 有效条目 */
};

static struct arp_entry arp_cache[ARP_CACHE_SIZE];

/* 本机 MAC 和 IP */
static uint8_t  my_mac[6];
static uint32_t my_ip;

/* ==================================================================
 * eth_init() — 初始化以太网/ARP 层
 *
 * 从 virtio-net 驱动获取 MAC 地址, 设置本机 IP。
 * ================================================================== */
void eth_init(void)
{
    uart_puts("[eth] Initializing Ethernet/ARP layer...\n");

    /* 获取 MAC 地址 */
    virtio_net_get_mac(my_mac);
    my_ip = MY_IP;

    /* 清空 ARP 缓存 */
    memset(arp_cache, 0, sizeof(arp_cache));

    uart_puts("[eth] Local IP: 10.0.2.2\n");
}

/* ==================================================================
 * arp_lookup() — 在 ARP 缓存中查找 IP 对应的 MAC
 *
 * 参数:
 *   ip  — 要查找的 IP 地址 (主机字节序)
 *   mac — 输出: 找到的 MAC 地址 (6 字节)
 *
 * 返回: 0 成功, -1 未找到
 * ================================================================== */
int arp_lookup(uint32_t ip, uint8_t *mac)
{
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            memcpy(mac, arp_cache[i].mac, 6);
            return 0;
        }
    }
    return -1;
}

/* ==================================================================
 * arp_insert() — 将 IP-MAC 映射插入 ARP 缓存
 * ================================================================== */
static void arp_insert(uint32_t ip, const uint8_t *mac)
{
    /* 查找已有条目或空槽 */
    int slot = -1;
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            slot = i;
            break;
        }
        if (!arp_cache[i].valid && slot == -1) {
            slot = i;
        }
    }
    if (slot == -1) slot = 0;  /* 覆盖第一个 (简单 LRU) */

    arp_cache[slot].ip = ip;
    memcpy(arp_cache[slot].mac, mac, 6);
    arp_cache[slot].valid = 1;
}

/* ==================================================================
 * arp_send_request() — 发送 ARP 请求
 *
 * 广播 ARP Request 以获取目标 IP 的 MAC 地址。
 *
 * 参数:
 *   target_ip — 要解析的 IP 地址 (主机字节序)
 *
 * 参考: [RFC826] "Packet Generation"
 * ================================================================== */
void arp_send_request(uint32_t target_ip)
{
    /* 构建 ARP + Ethernet 帧 */
    uint8_t frame[ETH_HLEN + sizeof(struct arphdr)];

    /* 以太网头: 目标 = 广播 FF:FF:FF:FF:FF:FF */
    struct ethhdr *eth = (struct ethhdr *)frame;
    memset(eth->dst, 0xFF, 6);          /* 广播地址 */
    memcpy(eth->src, my_mac, 6);        /* 源 = 本机 MAC */
    eth->type = htons(ETH_P_ARP);       /* EtherType = ARP */

    /* ARP 头 */
    struct arphdr *arp = (struct arphdr *)(frame + ETH_HLEN);
    arp->hw_type    = htons(ARP_HW_ETHERNET);   /* 硬件类型: Ethernet */
    arp->proto_type = htons(ETH_P_IP);           /* 协议类型: IPv4 */
    arp->hw_len     = 6;                         /* MAC 长度 */
    arp->proto_len  = 4;                         /* IP 长度 */
    arp->opcode     = htons(ARP_OP_REQUEST);     /* 操作: 请求 */
    memcpy(arp->sender_mac, my_mac, 6);          /* 发送方 MAC */
    arp->sender_ip  = htonl(my_ip);              /* 发送方 IP */
    memset(arp->target_mac, 0, 6);               /* 目标 MAC: 未知(全0) */
    arp->target_ip  = htonl(target_ip);          /* 目标 IP */

    /* 通过 virtio-net 发送帧 */
    virtio_net_send(frame, sizeof(frame));
    uart_puts("[arp] Sent ARP request for 10.0.2.1\n");
}

/* ==================================================================
 * arp_process() — 处理收到的 ARP 报文
 *
 * 参考: [RFC826] "Packet Reception"
 * ================================================================== */
void arp_process(const uint8_t *data, uint32_t len)
{
    if (len < sizeof(struct arphdr)) return;

    struct arphdr *arp = (struct arphdr *)data;

    /* 只处理 Ethernet + IPv4 的 ARP */
    if (ntohs(arp->hw_type) != ARP_HW_ETHERNET) return;
    if (ntohs(arp->proto_type) != ETH_P_IP) return;

    uint32_t sender_ip = ntohl(arp->sender_ip);

    /* 将发送方的 IP-MAC 映射存入缓存 */
    arp_insert(sender_ip, arp->sender_mac);

    uint16_t opcode = ntohs(arp->opcode);

    if (opcode == ARP_OP_REQUEST) {
        /* 如果目标 IP 是我们, 发送 ARP Reply
         * 参考: [RFC826] "If target matches my IP, send reply" */
        uint32_t target_ip = ntohl(arp->target_ip);
        if (target_ip == my_ip) {
            uart_puts("[arp] Replying to ARP request\n");

            uint8_t frame[ETH_HLEN + sizeof(struct arphdr)];
            struct ethhdr *reth = (struct ethhdr *)frame;
            memcpy(reth->dst, arp->sender_mac, 6);
            memcpy(reth->src, my_mac, 6);
            reth->type = htons(ETH_P_ARP);

            struct arphdr *rarp = (struct arphdr *)(frame + ETH_HLEN);
            rarp->hw_type    = htons(ARP_HW_ETHERNET);
            rarp->proto_type = htons(ETH_P_IP);
            rarp->hw_len     = 6;
            rarp->proto_len  = 4;
            rarp->opcode     = htons(ARP_OP_REPLY);
            memcpy(rarp->sender_mac, my_mac, 6);
            rarp->sender_ip  = htonl(my_ip);
            memcpy(rarp->target_mac, arp->sender_mac, 6);
            rarp->target_ip  = arp->sender_ip;

            virtio_net_send(frame, sizeof(frame));
        }
    } else if (opcode == ARP_OP_REPLY) {
        uart_puts("[arp] Received ARP reply\n");
        /* 已经在上面存入缓存了 */
    }
}

/* ==================================================================
 * eth_send() — 发送以太网帧
 *
 * 为 sk_buff 添加以太网头, 然后通过 virtio-net 发送。
 *
 * 参数:
 *   skb      — 包含上层数据 (如 IP 包) 的 sk_buff
 *   dst_mac  — 目标 MAC 地址
 *   ethertype — 以太网类型 (如 ETH_P_IP)
 *
 * 参考: [IEEE802.3] "MAC Frame Format"
 * ================================================================== */
void eth_send(struct sk_buff *skb, const uint8_t *dst_mac, uint16_t ethertype)
{
    /* 在 sk_buff 头部添加以太网头
     * skb_push 将 data 前移 ETH_HLEN(14) 字节
     * 参考: [ULP] "Adding Ethernet Header" */
    struct ethhdr *eth = (struct ethhdr *)skb_push(skb, ETH_HLEN);

    memcpy(eth->dst, dst_mac, ETH_ALEN);    /* 填入目标 MAC */
    memcpy(eth->src, my_mac, ETH_ALEN);     /* 填入源 MAC (本机) */
    eth->type = htons(ethertype);            /* 填入 EtherType */

    /* 通过 virtio-net 发送完整帧 (包含以太网头 + 上层数据) */
    virtio_net_send(skb->data, skb->len);
}

/* ==================================================================
 * eth_get_mac() — 获取本机 MAC 地址
 * ================================================================== */
void eth_get_mac(uint8_t *mac)
{
    memcpy(mac, my_mac, 6);
}

/* ==================================================================
 * eth_get_ip() — 获取本机 IP 地址
 * ================================================================== */
uint32_t eth_get_ip(void)
{
    return my_ip;
}
