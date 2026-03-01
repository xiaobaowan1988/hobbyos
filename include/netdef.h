/*
 * include/netdef.h — 网络协议结构和常量定义
 *
 * 定义以太网、ARP、IP、TCP、UDP 等协议的头部结构和常量。
 * 所有结构使用 __attribute__((packed)) 确保无填充字节。
 *
 * 参考文档:
 *   [IEEE802.3] IEEE 802.3 Ethernet Standard
 *   [RFC826]  ARP — An Ethernet Address Resolution Protocol
 *   [RFC791]  IP — Internet Protocol
 *   [RFC793]  TCP — Transmission Control Protocol
 *   [RFC768]  UDP — User Datagram Protocol
 */

#ifndef NETDEF_H
#define NETDEF_H

#include "types.h"

/* ==================================================================
 * 字节序转换宏
 *
 * 网络协议使用大端 (Big-Endian) 字节序。
 * ARM64 默认运行在小端 (Little-Endian) 模式。
 * 需要在主机字节序和网络字节序之间转换。
 *
 * 参考: [RFC791] "All fields are in network byte order (big-endian)"
 * ================================================================== */

/* htons — Host to Network Short (16-bit) */
static inline uint16_t htons(uint16_t x)
{
    return ((x & 0xFF) << 8) | ((x >> 8) & 0xFF);
}

/* ntohs — Network to Host Short (16-bit) */
static inline uint16_t ntohs(uint16_t x)
{
    return htons(x);  /* 同一操作 */
}

/* htonl — Host to Network Long (32-bit) */
static inline uint32_t htonl(uint32_t x)
{
    return ((x & 0xFF) << 24) |
           ((x & 0xFF00) << 8) |
           ((x >> 8) & 0xFF00) |
           ((x >> 24) & 0xFF);
}

/* ntohl — Network to Host Long (32-bit) */
static inline uint32_t ntohl(uint32_t x)
{
    return htonl(x);
}

/* ==================================================================
 * 以太网 (Ethernet II) 帧头
 *
 * 以太网帧格式:
 *   | Dst MAC (6) | Src MAC (6) | EtherType (2) | Payload (46-1500) |
 *
 * 参考: [IEEE802.3] Section 3.1.1 "MAC Frame Format"
 * ================================================================== */
#define ETH_ALEN        6       /* MAC 地址长度 (字节) */
#define ETH_HLEN        14      /* 以太网头长度 (6+6+2) */
#define ETH_P_IP        0x0800  /* EtherType: IPv4 */
#define ETH_P_ARP       0x0806  /* EtherType: ARP */

struct ethhdr {
    uint8_t  dst[ETH_ALEN];     /* 目标 MAC 地址 */
    uint8_t  src[ETH_ALEN];     /* 源 MAC 地址 */
    uint16_t type;              /* EtherType (网络字节序) */
} __attribute__((packed));

/* ==================================================================
 * ARP (Address Resolution Protocol) — 地址解析协议
 *
 * ARP 用于将 IP 地址解析为 MAC 地址。
 * 当主机需要发送 IP 包但不知道目标 MAC 时, 发送 ARP 请求广播。
 *
 * ARP 报文格式 (以太网 + IPv4):
 *   | HW Type (2) | Proto Type (2) | HW Len (1) | Proto Len (1) |
 *   | Opcode (2) | Sender MAC (6) | Sender IP (4) |
 *   | Target MAC (6) | Target IP (4) |
 *
 * 参考: [RFC826] "An Ethernet Address Resolution Protocol"
 * ================================================================== */
#define ARP_HW_ETHERNET 1       /* Hardware type: Ethernet */
#define ARP_OP_REQUEST  1       /* ARP 请求 */
#define ARP_OP_REPLY    2       /* ARP 响应 */

struct arphdr {
    uint16_t hw_type;           /* 硬件类型 (1 = Ethernet) */
    uint16_t proto_type;        /* 协议类型 (0x0800 = IPv4) */
    uint8_t  hw_len;            /* 硬件地址长度 (6 for MAC) */
    uint8_t  proto_len;         /* 协议地址长度 (4 for IPv4) */
    uint16_t opcode;            /* 操作码 (1=Request, 2=Reply) */
    uint8_t  sender_mac[6];     /* 发送方 MAC */
    uint32_t sender_ip;         /* 发送方 IP (网络字节序) */
    uint8_t  target_mac[6];     /* 目标 MAC */
    uint32_t target_ip;         /* 目标 IP (网络字节序) */
} __attribute__((packed));

/* ==================================================================
 * IPv4 头部
 *
 * 参考: [RFC791] "Internet Header Format"
 * ================================================================== */
#define IP_PROTO_ICMP   1       /* ICMP 协议号 */
#define IP_PROTO_TCP    6       /* TCP 协议号 */
#define IP_PROTO_UDP    17      /* UDP 协议号 */

struct iphdr {
    uint8_t  ver_ihl;           /* [7:4]版本=4 [3:0]头部长度(×4字节) */
    uint8_t  tos;               /* 服务类型 */
    uint16_t tot_len;           /* 总长度 (含头部, 网络字节序) */
    uint16_t id;                /* 标识 (分片用) */
    uint16_t frag_off;          /* 标志 + 分片偏移 */
    uint8_t  ttl;               /* 生存时间 */
    uint8_t  protocol;          /* 上层协议 (6=TCP, 17=UDP) */
    uint16_t checksum;          /* 头部校验和 */
    uint32_t src_ip;            /* 源 IP 地址 (网络字节序) */
    uint32_t dst_ip;            /* 目标 IP 地址 (网络字节序) */
} __attribute__((packed));

/* ==================================================================
 * TCP 头部
 *
 * 参考: [RFC793] "TCP Header Format"
 * ================================================================== */
#define TCP_FIN     0x01
#define TCP_SYN     0x02
#define TCP_RST     0x04
#define TCP_PSH     0x08
#define TCP_ACK     0x10

struct tcphdr {
    uint16_t src_port;          /* 源端口号 */
    uint16_t dst_port;          /* 目标端口号 */
    uint32_t seq;               /* 序列号 */
    uint32_t ack_seq;           /* 确认号 */
    uint8_t  data_off;          /* [7:4]数据偏移(×4字节) [3:0]保留 */
    uint8_t  flags;             /* 控制标志 (SYN, ACK, FIN, ...) */
    uint16_t window;            /* 窗口大小 */
    uint16_t checksum;          /* 校验和 */
    uint16_t urgent;            /* 紧急指针 */
} __attribute__((packed));

/* ==================================================================
 * UDP 头部
 *
 * 参考: [RFC768] "User Datagram Protocol"
 * ================================================================== */
struct udphdr {
    uint16_t src_port;          /* 源端口号 */
    uint16_t dst_port;          /* 目标端口号 */
    uint16_t len;               /* UDP 长度 (含头部 8 字节) */
    uint16_t checksum;          /* 校验和 (可选, 0=不使用) */
} __attribute__((packed));

/* ==================================================================
 * 网络配置常量 (静态配置, 简单起见不实现 DHCP)
 * ================================================================== */
#define MY_IP           0x0A000202UL    /* 10.0.2.2 (网络字节序需转换) */
#define GATEWAY_IP      0x0A000201UL    /* 10.0.2.1 (QEMU 默认网关) */
#define SUBNET_MASK     0xFFFFFF00UL    /* 255.255.255.0 */

/* IP 地址辅助宏: IP4(10,0,2,2) = 0x0A000202 (主机字节序) */
#define IP4(a,b,c,d) (((uint32_t)(a)<<24)|((uint32_t)(b)<<16)|((uint32_t)(c)<<8)|(uint32_t)(d))

#endif /* NETDEF_H */
