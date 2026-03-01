/*
 * include/skbuff.h — 网络缓冲区 (sk_buff) 头文件
 *
 * sk_buff 是网络协议栈的核心数据结构, 每个网络数据包用一个 sk_buff 表示。
 * 它管理数据包的内存, 支持在头部/尾部添加协议头 (header push/pull)。
 *
 * 类比 Linux 内核的 struct sk_buff (include/linux/skbuff.h)。
 * 我们的实现是极简版本, 仅保留核心功能。
 *
 * 参考文档:
 *   [LINUX-NET] Linux 内核 net/ 目录下的网络协议栈
 *   [ULP] Understanding Linux Networking Internals, Chapter 2 "sk_buff"
 */

#ifndef SKBUFF_H
#define SKBUFF_H

#include "types.h"

/* ==================================================================
 * sk_buff — 网络缓冲区结构
 *
 * 内存布局:
 *   head ────────── 缓冲区起始 (固定不变)
 *     │ (headroom — 预留空间, 用于后续添加 L2/L3/L4 头)
 *   data ────────── 数据起始指针 (随 push/pull 移动)
 *     │ (有效数据)
 *   tail ────────── 数据结束指针
 *     │ (tailroom — 尾部预留空间)
 *   end  ────────── 缓冲区结束 (固定不变)
 *
 * 协议头添加流程 (以发送为例):
 *   1. 应用层: 放入 payload ("Hello World")
 *   2. TCP 层: skb_push(tcphdr_size) → data 前移, 填入 TCP 头
 *   3. IP 层:  skb_push(iphdr_size) → data 再前移, 填入 IP 头
 *   4. 以太网: skb_push(ethhdr_size) → data 再前移, 填入以太网头
 *
 * 参考: [ULP] Chapter 2 "The Socket Buffer: sk_buff Structure"
 * ================================================================== */

#define SKB_MAX_SIZE    2048    /* 单个 sk_buff 缓冲区的最大大小 */
#define SKB_HEADROOM    128     /* 默认头部预留空间 (足够放所有协议头) */

struct sk_buff {
    uint8_t  *head;     /* 缓冲区起始 (固定) */
    uint8_t  *data;     /* 有效数据起始 (可变) */
    uint8_t  *tail;     /* 有效数据结束 (可变) */
    uint8_t  *end;      /* 缓冲区结束 (固定) */

    uint32_t  len;      /* 有效数据长度 = tail - data */

    /* 协议相关字段 */
    uint16_t  protocol; /* 以太网类型 (如 0x0800=IP, 0x0806=ARP) */

    /* 存储缓冲区 (内联在结构体末尾, 避免额外分配) */
    uint8_t   buf[SKB_MAX_SIZE];
};

/* ==================================================================
 * sk_buff 操作函数
 * ================================================================== */

/* skb_alloc() — 分配一个 sk_buff
 *
 * 初始化 head/data/tail/end 指针, data 从 headroom 之后开始。
 * 返回: 新分配的 sk_buff 指针, 或 NULL */
struct sk_buff *skb_alloc(void);

/* skb_free() — 释放一个 sk_buff */
void skb_free(struct sk_buff *skb);

/* skb_put() — 在尾部追加数据空间
 *
 * tail 后移 len 字节, 返回原 tail 位置。
 * 调用者可向返回的指针写入数据。
 *
 * 参数: len — 要追加的字节数
 * 返回: 新数据区域的起始指针 */
uint8_t *skb_put(struct sk_buff *skb, uint32_t len);

/* skb_push() — 在头部预留/添加协议头空间
 *
 * data 前移 len 字节, 返回新 data 位置。
 * 调用者可向返回的指针写入协议头。
 *
 * 参数: len — 协议头大小
 * 返回: 新 data 指针 (协议头写入位置) */
uint8_t *skb_push(struct sk_buff *skb, uint32_t len);

/* skb_pull() — 从头部移除数据 (跳过协议头)
 *
 * data 后移 len 字节 (解析接收帧时使用)。
 *
 * 参数: len — 要跳过的字节数
 * 返回: 新 data 指针 */
uint8_t *skb_pull(struct sk_buff *skb, uint32_t len);

#endif /* SKBUFF_H */
