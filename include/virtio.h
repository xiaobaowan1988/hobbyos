/*
 * include/virtio.h — virtio 设备抽象和 virtio-net 驱动头文件
 *
 * virtio 是 QEMU/KVM 虚拟化环境下的标准半虚拟化设备接口。
 * 相比模拟真实硬件, virtio 提供更简洁高效的 guest-host 通信。
 *
 * virtio 通过 "virtqueue" (虚拟队列) 在 guest 和 host 之间传递数据。
 * 每个 virtqueue 由三部分组成:
 *   1. Descriptor Table — 描述数据缓冲区的地址/长度/标志
 *   2. Available Ring  — Guest → Host: Guest 通知 Host 有新数据
 *   3. Used Ring       — Host → Guest: Host 通知 Guest 数据已处理
 *
 * 参考文档:
 *   [VIRTIO-SPEC] Virtual I/O Device (VIRTIO) Specification v1.1
 *   [VIRTIO-SPEC] 2.6 "Virtqueues"
 *   [VIRTIO-SPEC] 4.1 "Virtio Over PCI Bus"
 *   [VIRTIO-SPEC] 5.1 "Network Device"
 */

#ifndef VIRTIO_H
#define VIRTIO_H

#include "types.h"
#include "pci.h"

/* ==================================================================
 * virtio PCI Vendor/Device ID
 *
 * virtio 设备通过 PCI 发现, Vendor ID 固定为 0x1AF4 (Red Hat)。
 * Device ID:
 *   0x1000 = Network card (legacy, transitional)
 *   0x1001 = Block device (legacy)
 *   ...
 *
 * 参考: [VIRTIO-SPEC] 4.1.2 "PCI Device Discovery"
 * ================================================================== */
#define VIRTIO_VENDOR_ID    0x1AF4
#define VIRTIO_NET_DEV_ID   0x1000  /* virtio-net (legacy/transitional) */

/* ==================================================================
 * virtio legacy I/O 寄存器偏移 (PCI legacy interface)
 *
 * 当使用 legacy interface 时, virtio 寄存器映射到 BAR0。
 *
 * 参考: [VIRTIO-SPEC] 4.1.4.8 "Legacy Interfaces: A Note on PCI Device Layout"
 * ================================================================== */
#define VIRTIO_REG_DEVICE_FEATURES  0x00  /* 32-bit: 设备支持的特性 (只读) */
#define VIRTIO_REG_DRIVER_FEATURES  0x04  /* 32-bit: 驱动接受的特性 (读写) */
#define VIRTIO_REG_QUEUE_ADDR       0x08  /* 32-bit: virtqueue 物理地址 / 4096 */
#define VIRTIO_REG_QUEUE_SIZE       0x0C  /* 16-bit: virtqueue 大小 (描述符数) */
#define VIRTIO_REG_QUEUE_SELECT     0x0E  /* 16-bit: 选择当前 virtqueue */
#define VIRTIO_REG_QUEUE_NOTIFY     0x10  /* 16-bit: 通知设备 (写队列号) */
#define VIRTIO_REG_DEVICE_STATUS    0x12  /* 8-bit: 设备状态 */
#define VIRTIO_REG_ISR_STATUS       0x13  /* 8-bit: 中断状态 */
#define VIRTIO_REG_NET_MAC          0x14  /* 6 bytes: MAC 地址 (net-specific) */

/* ==================================================================
 * virtio 设备状态位
 *
 * 驱动通过依次设置这些状态位来完成设备初始化握手。
 * 参考: [VIRTIO-SPEC] 2.1 "Device Status Field"
 * ================================================================== */
#define VIRTIO_STATUS_ACKNOWLEDGE   1    /* OS 已发现此设备 */
#define VIRTIO_STATUS_DRIVER        2    /* OS 知道如何驱动此设备 */
#define VIRTIO_STATUS_DRIVER_OK     4    /* 驱动已就绪 */
#define VIRTIO_STATUS_FEATURES_OK   8    /* 特性协商完成 */

/* ==================================================================
 * virtqueue 描述符标志
 *
 * 参考: [VIRTIO-SPEC] 2.6.5 "The Virtqueue Descriptor Table"
 * ================================================================== */
#define VRING_DESC_F_NEXT     1   /* 此描述符链接到下一个 (desc.next 有效) */
#define VRING_DESC_F_WRITE    2   /* 此缓冲区由设备写入 (用于接收) */

/* ==================================================================
 * virtqueue 常量
 * ================================================================== */
#define VIRTQ_SIZE  256  /* 每个 virtqueue 的描述符数量 */

/* ==================================================================
 * virtqueue 描述符 (Descriptor)
 *
 * 每个描述符描述一块内存缓冲区。
 *
 * 参考: [VIRTIO-SPEC] 2.6.5 "The Virtqueue Descriptor Table"
 *        struct vring_desc
 * ================================================================== */
struct virtq_desc {
    uint64_t addr;      /* 缓冲区物理地址 */
    uint32_t len;       /* 缓冲区长度 (字节) */
    uint16_t flags;     /* 标志: NEXT, WRITE, INDIRECT */
    uint16_t next;      /* 链接的下一个描述符索引 */
} __attribute__((packed));

/* ==================================================================
 * virtqueue Available Ring (Guest → Host)
 *
 * Guest 将可用的描述符索引放入此环,
 * 然后通知 Host (写 QUEUE_NOTIFY) 来触发处理。
 *
 * 参考: [VIRTIO-SPEC] 2.6.6 "The Virtqueue Available Ring"
 * ================================================================== */
struct virtq_avail {
    uint16_t flags;             /* 0 = 正常, 1 = 不需要中断通知 */
    uint16_t idx;               /* 下一个写入位置 (单调递增) */
    uint16_t ring[VIRTQ_SIZE];  /* 描述符索引环形数组 */
} __attribute__((packed));

/* ==================================================================
 * virtqueue Used Ring (Host → Guest)
 *
 * Host 处理完描述符后, 将结果放入此环。
 * Guest 通过比较 used.idx 和上次值来检测新完成的项。
 *
 * 参考: [VIRTIO-SPEC] 2.6.8 "The Virtqueue Used Ring"
 * ================================================================== */
struct virtq_used_elem {
    uint32_t id;    /* 完成的描述符链头索引 */
    uint32_t len;   /* 设备写入的字节数 */
} __attribute__((packed));

struct virtq_used {
    uint16_t flags;
    uint16_t idx;                       /* 下一个写入位置 */
    struct virtq_used_elem ring[VIRTQ_SIZE];
} __attribute__((packed));

/* ==================================================================
 * virtqueue 完整结构
 *
 * 包含描述符表、可用环、已用环, 以及驱动侧的管理状态。
 * ================================================================== */
struct virtqueue {
    struct virtq_desc  *desc;       /* 描述符表 (设备和驱动共享) */
    struct virtq_avail *avail;      /* 可用环 (驱动写, 设备读) */
    struct virtq_used  *used;       /* 已用环 (设备写, 驱动读) */

    uint16_t num;                   /* 描述符总数 */
    uint16_t free_head;             /* 空闲描述符链表头 */
    uint16_t last_used_idx;         /* 驱动上次处理到的 used.idx */
};

/* ==================================================================
 * virtio-net 设备结构
 * ================================================================== */
struct virtio_net_dev {
    volatile uint8_t *base;         /* BAR0 MMIO 基地址 */
    struct pci_device *pci;         /* PCI 设备信息 */

    struct virtqueue rx_vq;         /* 接收队列 (virtqueue 0) */
    struct virtqueue tx_vq;         /* 发送队列 (virtqueue 1) */

    uint8_t mac[6];                 /* MAC 地址 */
};

/* ==================================================================
 * virtio-net 接口函数
 * ================================================================== */

/* virtio_net_init() — 初始化 virtio-net 设备
 * 返回: 0 成功, -1 失败 (设备未找到) */
int virtio_net_init(void);

/* virtio_net_send() — 发送一帧以太网数据
 * 参数:
 *   data — 以太网帧数据 (含以太网头)
 *   len  — 数据长度
 * 返回: 0 成功, -1 失败 */
int virtio_net_send(const void *data, uint32_t len);

/* virtio_net_recv() — 接收一帧以太网数据 (阻塞)
 * 参数:
 *   buf     — 接收缓冲区
 *   buf_len — 缓冲区大小
 * 返回: 接收到的字节数, 或 -1 失败 */
int virtio_net_recv(void *buf, uint32_t buf_len);

/* virtio_net_get_mac() — 获取 MAC 地址
 * 参数: mac — 6 字节输出缓冲区 */
void virtio_net_get_mac(uint8_t *mac);

#endif /* VIRTIO_H */
