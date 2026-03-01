/*
 * include/net.h — 网络协议栈接口
 *
 * 统一的网络层接口头文件, 声明各层的公开函数。
 */

#ifndef NET_H
#define NET_H

#include "types.h"
#include "skbuff.h"

/* ==================================================================
 * 以太网层 (L2)
 * ================================================================== */
void     eth_init(void);
void     eth_send(struct sk_buff *skb, const uint8_t *dst_mac, uint16_t ethertype);
void     eth_get_mac(uint8_t *mac);
uint32_t eth_get_ip(void);

/* ==================================================================
 * ARP
 * ================================================================== */
int  arp_lookup(uint32_t ip, uint8_t *mac);
void arp_send_request(uint32_t target_ip);
void arp_process(const uint8_t *data, uint32_t len);

/* ==================================================================
 * IP 层 (L3)
 * ================================================================== */
void ip_send(struct sk_buff *skb, uint32_t dst_ip, uint8_t protocol);
void ip_recv(const uint8_t *data, uint32_t len);

/* ==================================================================
 * UDP 层 (L4)
 * ================================================================== */
void udp_send(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
              const void *data, uint32_t len);
void udp_recv(uint32_t src_ip, const uint8_t *data, uint32_t len);

/* ==================================================================
 * TCP 层 (L4)
 * ================================================================== */
int  tcp_connect(uint32_t dst_ip, uint16_t dst_port);
int  tcp_send(int sock, const void *data, uint32_t len);
void tcp_recv(uint32_t src_ip, const uint8_t *data, uint32_t len);
void tcp_close(int sock);

#endif /* NET_H */
