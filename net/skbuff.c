/*
 * net/skbuff.c — 网络缓冲区 (sk_buff) 实现
 *
 * sk_buff 管理网络数据包的生命周期和内存布局。
 * 核心设计: 通过移动 data/tail 指针来添加/移除协议头,
 * 避免数据拷贝, 提高性能。
 *
 * 参考文档:
 *   [ULP] Understanding Linux Networking Internals, Chapter 2
 *   [LINUX-NET] Linux net/core/skbuff.c
 */

#include "skbuff.h"
#include "mm.h"
#include "string.h"
#include "uart.h"

/* ==================================================================
 * 简单 sk_buff 池 — 静态分配
 *
 * 真实 OS 使用 slab allocator 动态分配 sk_buff。
 * 我们使用简单的静态数组 + 空闲标志。
 * ================================================================== */
#define SKB_POOL_SIZE   32

static struct sk_buff skb_pool[SKB_POOL_SIZE];
static uint8_t        skb_used[SKB_POOL_SIZE];  /* 0=空闲, 1=已分配 */

/* ==================================================================
 * skb_alloc() — 分配一个 sk_buff
 *
 * 从池中找到空闲 sk_buff, 初始化指针。
 *
 * 初始状态:
 *   head = buf 起始
 *   data = head + SKB_HEADROOM (留出头部空间)
 *   tail = data (初始无有效数据)
 *   end  = buf + SKB_MAX_SIZE
 *
 * 参考: [LINUX-NET] alloc_skb()
 * ================================================================== */
struct sk_buff *skb_alloc(void)
{
    for (int i = 0; i < SKB_POOL_SIZE; i++) {
        if (!skb_used[i]) {
            skb_used[i] = 1;
            struct sk_buff *skb = &skb_pool[i];

            /* 初始化指针
             * head 和 end 是固定边界, data 和 tail 会随操作移动。 */
            skb->head = skb->buf;
            skb->end  = skb->buf + SKB_MAX_SIZE;
            skb->data = skb->head + SKB_HEADROOM;  /* 预留头部空间 */
            skb->tail = skb->data;                  /* 初始无数据 */
            skb->len  = 0;
            skb->protocol = 0;

            return skb;
        }
    }
    uart_puts("[skb] ERROR: skb pool exhausted!\n");
    return NULL;
}

/* ==================================================================
 * skb_free() — 释放 sk_buff 回到池中
 *
 * 参考: [LINUX-NET] kfree_skb()
 * ================================================================== */
void skb_free(struct sk_buff *skb)
{
    /* 计算池中的索引 */
    int idx = skb - skb_pool;
    if (idx >= 0 && idx < SKB_POOL_SIZE) {
        skb_used[idx] = 0;
    }
}

/* ==================================================================
 * skb_put() — 在尾部追加数据空间
 *
 * 移动 tail 指针, 增加有效数据长度。
 * 用于: 应用层放入 payload 数据。
 *
 * 返回: 原 tail 位置 (调用者向此处写入数据)
 *
 * 参考: [LINUX-NET] skb_put()
 * ================================================================== */
uint8_t *skb_put(struct sk_buff *skb, uint32_t len)
{
    uint8_t *old_tail = skb->tail;

    skb->tail += len;              /* tail 后移 */
    skb->len  += len;              /* 有效数据长度增加 */

    return old_tail;
}

/* ==================================================================
 * skb_push() — 在头部添加协议头空间
 *
 * 移动 data 指针向前 (减小), 腾出空间写入协议头。
 * 用于: TCP/IP/Ethernet 层逐层添加协议头。
 *
 * 返回: 新 data 位置 (调用者向此处写入协议头)
 *
 * 参考: [LINUX-NET] skb_push()
 * ================================================================== */
uint8_t *skb_push(struct sk_buff *skb, uint32_t len)
{
    skb->data -= len;              /* data 前移 (减小地址) */
    skb->len  += len;              /* 有效数据长度增加 (含新的头部) */

    return skb->data;
}

/* ==================================================================
 * skb_pull() — 从头部移除数据 (跳过协议头)
 *
 * 移动 data 指针向后 (增大), 跳过已处理的协议头。
 * 用于: 接收路径, 解析并剥离每层协议头。
 *
 * 返回: 新 data 位置
 *
 * 参考: [LINUX-NET] skb_pull()
 * ================================================================== */
uint8_t *skb_pull(struct sk_buff *skb, uint32_t len)
{
    skb->data += len;              /* data 后移 (增大地址) */
    skb->len  -= len;              /* 有效数据长度减少 */

    return skb->data;
}
