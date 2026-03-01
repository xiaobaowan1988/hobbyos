/*
 * drivers/virtio_net.c — virtio-net 网卡驱动
 *
 * 实现 virtio legacy PCI 接口的网络设备驱动。
 * 支持通过 virtqueue 发送和接收以太网帧。
 *
 * virtio-net 使用两个 virtqueue:
 *   Queue 0: 接收 (RX) — Host 将收到的帧放入此队列
 *   Queue 1: 发送 (TX) — Guest 将要发送的帧放入此队列
 *
 * 参考文档:
 *   [VIRTIO-SPEC] Virtual I/O Device (VIRTIO) Specification v1.1
 *   [VIRTIO-SPEC] 5.1 "Network Device"
 */

#include "virtio.h"
#include "mm.h"
#include "gic.h"
#include "uart.h"
#include "string.h"

/* ==================================================================
 * 全局 virtio-net 设备实例
 * ================================================================== */
static struct virtio_net_dev net_dev;

/* ==================================================================
 * MMIO 读写辅助函数
 *
 * virtio legacy 接口的寄存器通过 BAR0 的 MMIO 访问。
 * ================================================================== */
static uint32_t vio_read32(uint16_t off)
{
    return *(volatile uint32_t *)(net_dev.base + off);
}
static void vio_write32(uint16_t off, uint32_t val)
{
    *(volatile uint32_t *)(net_dev.base + off) = val;
}
static uint16_t vio_read16(uint16_t off)
{
    return *(volatile uint16_t *)(net_dev.base + off);
}
static void vio_write16(uint16_t off, uint16_t val)
{
    *(volatile uint16_t *)(net_dev.base + off) = val;
}
static uint8_t vio_read8(uint16_t off)
{
    return *(volatile uint8_t *)(net_dev.base + off);
}
static void vio_write8(uint16_t off, uint8_t val)
{
    *(volatile uint8_t *)(net_dev.base + off) = val;
}

/* ==================================================================
 * 辅助: 打印 MAC 地址
 * ================================================================== */
static void print_mac(const uint8_t *mac)
{
    const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 6; i++) {
        if (i > 0) uart_putc(':');
        uart_putc(hex[(mac[i] >> 4) & 0xF]);
        uart_putc(hex[mac[i] & 0xF]);
    }
}

/* ==================================================================
 * virtq_init() — 初始化一个 virtqueue
 *
 * 分配物理内存并设置 descriptor table, available ring, used ring。
 *
 * virtqueue 的内存布局 (必须按特定规则对齐):
 *   Descriptor Table: num × 16 字节
 *   Available Ring:   6 + 2 × num 字节
 *   (对齐到 4096)
 *   Used Ring:        6 + 8 × num 字节
 *
 * 参考: [VIRTIO-SPEC] 2.6.2 "Legacy Interfaces: A Note on Virtqueue Layout"
 *
 * 参数:
 *   vq       — 要初始化的 virtqueue
 *   queue_idx — 队列索引 (0=RX, 1=TX)
 * ================================================================== */
static void virtq_init(struct virtqueue *vq, uint16_t queue_idx)
{
    /* 步骤 1: 选择队列
     * 写入 QUEUE_SELECT 选择要配置的队列。
     * 参考: [VIRTIO-SPEC] 4.1.4.3.1 "Queue Select" */
    vio_write16(VIRTIO_REG_QUEUE_SELECT, queue_idx);

    /* 步骤 2: 读取队列大小
     * QUEUE_SIZE 返回设备支持的最大描述符数。
     * 如果为 0 表示此队列不可用。
     * 参考: [VIRTIO-SPEC] 4.1.4.3.2 "Queue Size" */
    uint16_t size = vio_read16(VIRTIO_REG_QUEUE_SIZE);
    if (size == 0) {
        uart_puts("[virtio] Queue ");
        uart_putc('0' + queue_idx);
        uart_puts(" not available!\n");
        return;
    }
    if (size > VIRTQ_SIZE) size = VIRTQ_SIZE;
    vq->num = size;

    /* 步骤 3: 分配 virtqueue 内存
     *
     * Legacy 布局需要一块连续物理内存:
     *   offset 0:    Descriptor Table (16 × num)
     *   offset desc: Available Ring (6 + 2 × num)
     *   对齐到 4096 后: Used Ring (6 + 8 × num)
     *
     * 简单起见分配 2 个 4KB 页面 (8KB, 足够 256 个描述符)。
     * 参考: [VIRTIO-SPEC] 2.6.2 */
    uint64_t page1 = page_alloc();
    uint64_t page2 = page_alloc();
    if (page1 == 0 || page2 == 0) {
        uart_puts("[virtio] Out of memory for virtqueue!\n");
        return;
    }

    /* 清零内存 */
    memset((void *)page1, 0, PAGE_SIZE);
    memset((void *)page2, 0, PAGE_SIZE);

    /* 设置各组件指针 */
    vq->desc  = (struct virtq_desc *)page1;
    vq->avail = (struct virtq_avail *)(page1 + size * sizeof(struct virtq_desc));
    vq->used  = (struct virtq_used *)page2;

    /* 步骤 4: 初始化空闲描述符链表
     * 将所有描述符链成一个单链表, 方便分配。 */
    for (uint16_t i = 0; i < size; i++) {
        vq->desc[i].next = i + 1;
    }
    vq->free_head = 0;
    vq->last_used_idx = 0;

    /* 步骤 5: 告诉设备 virtqueue 的物理地址
     *
     * Legacy 接口: QUEUE_ADDR = 物理地址 / 4096 (即页帧号)。
     * 设备据此定位 descriptor table, available ring, used ring。
     *
     * 参考: [VIRTIO-SPEC] 4.1.4.3.4 "Queue Address" */
    vio_write32(VIRTIO_REG_QUEUE_ADDR, (uint32_t)(page1 / PAGE_SIZE));
}

/* ==================================================================
 * virtio_net_init() — 初始化 virtio-net 设备
 *
 * 遵循 virtio 规范的设备初始化流程:
 *   1. Reset 设备 (写 status = 0)
 *   2. 设置 ACKNOWLEDGE 位
 *   3. 设置 DRIVER 位
 *   4. 读取/协商设备特性
 *   5. 设置 FEATURES_OK
 *   6. 初始化 virtqueue
 *   7. 设置 DRIVER_OK — 设备开始工作
 *
 * 参考: [VIRTIO-SPEC] 3.1 "Device Initialization"
 * ================================================================== */
int virtio_net_init(void)
{
    uart_puts("[virtio-net] Looking for virtio network device...\n");

    /* 步骤 1: 通过 PCI 查找 virtio-net 设备
     * Vendor ID = 0x1AF4 (Red Hat/virtio)
     * Device ID = 0x1000 (network, legacy)
     * 参考: [VIRTIO-SPEC] 4.1.2 "PCI Device Discovery" */
    struct pci_device *pci = pci_find_device(VIRTIO_VENDOR_ID, VIRTIO_NET_DEV_ID);
    if (pci == NULL) {
        uart_puts("[virtio-net] Device not found!\n");
        return -1;
    }

    net_dev.pci = pci;
    net_dev.base = (volatile uint8_t *)pci->bar0;

    uart_puts("[virtio-net] Found device at PCI ");
    uart_putc('0' + pci->bus);
    uart_puts(":");
    uart_putc('0' + pci->dev);
    uart_puts(".");
    uart_putc('0' + pci->func);
    uart_puts(", BAR0=0x");
    /* 打印 BAR0 地址 */
    {
        const char hex[] = "0123456789abcdef";
        uint64_t v = pci->bar0;
        char buf[17];
        for (int i = 15; i >= 0; i--) { buf[i] = hex[v & 0xF]; v >>= 4; }
        buf[16] = '\0';
        uart_puts(buf);
    }
    uart_puts("\n");

    /* 步骤 2: Reset 设备
     * 写 status = 0 触发设备复位。
     * 参考: [VIRTIO-SPEC] 3.1 步骤 1 "Reset the device" */
    vio_write8(VIRTIO_REG_DEVICE_STATUS, 0);

    /* 步骤 3: 设置 ACKNOWLEDGE — "我已发现此 virtio 设备"
     * 参考: [VIRTIO-SPEC] 3.1 步骤 2 */
    vio_write8(VIRTIO_REG_DEVICE_STATUS,
               vio_read8(VIRTIO_REG_DEVICE_STATUS) | VIRTIO_STATUS_ACKNOWLEDGE);

    /* 步骤 4: 设置 DRIVER — "我知道如何驱动此设备"
     * 参考: [VIRTIO-SPEC] 3.1 步骤 3 */
    vio_write8(VIRTIO_REG_DEVICE_STATUS,
               vio_read8(VIRTIO_REG_DEVICE_STATUS) | VIRTIO_STATUS_DRIVER);

    /* 步骤 5: 特性协商
     *
     * 读取设备支持的特性, 选择驱动需要的特性子集。
     * 简化起见我们不启用任何高级特性 (如 checksum offload)。
     *
     * 参考: [VIRTIO-SPEC] 3.1 步骤 4 "Read device feature bits"
     *        [VIRTIO-SPEC] 5.1.3 "Feature bits" */
    uint32_t features = vio_read32(VIRTIO_REG_DEVICE_FEATURES);
    (void)features;
    /* 接受最小特性集 (不启用任何高级特性) */
    vio_write32(VIRTIO_REG_DRIVER_FEATURES, 0);

    /* 步骤 6: 设置 FEATURES_OK
     * 参考: [VIRTIO-SPEC] 3.1 步骤 5-6 */
    vio_write8(VIRTIO_REG_DEVICE_STATUS,
               vio_read8(VIRTIO_REG_DEVICE_STATUS) | VIRTIO_STATUS_FEATURES_OK);

    /* 步骤 7: 初始化 virtqueue
     *
     * virtio-net 标准定义:
     *   Queue 0 = receiveq  (接收队列)
     *   Queue 1 = transmitq (发送队列)
     *
     * 参考: [VIRTIO-SPEC] 5.1.2 "Virtqueues" */
    virtq_init(&net_dev.rx_vq, 0);    /* 初始化接收队列 */
    virtq_init(&net_dev.tx_vq, 1);    /* 初始化发送队列 */

    /* 步骤 8: 读取 MAC 地址
     *
     * virtio-net legacy 接口: MAC 地址位于 BAR0 偏移 0x14 (6 字节)。
     * 参考: [VIRTIO-SPEC] 5.1.4 "Device configuration layout" */
    for (int i = 0; i < 6; i++) {
        net_dev.mac[i] = vio_read8(VIRTIO_REG_NET_MAC + i);
    }
    uart_puts("[virtio-net] MAC address: ");
    print_mac(net_dev.mac);
    uart_puts("\n");

    /* 步骤 9: 向接收队列填充缓冲区
     *
     * 预分配接收缓冲区, 让设备可以随时将收到的帧放入。
     * 每个接收缓冲区 = virtio-net header (10 bytes) + 最大以太网帧 (1514 bytes)。
     * 简化: 使用固定大小 2048 字节缓冲区。
     *
     * 参考: [VIRTIO-SPEC] 5.1.6.3 "Setting Up Receive Buffers" */
    {
        struct virtqueue *vq = &net_dev.rx_vq;
        int num_rx_bufs = 64;  /* 预分配 64 个接收缓冲区 */
        if (num_rx_bufs > vq->num / 2) num_rx_bufs = vq->num / 2;

        for (int i = 0; i < num_rx_bufs; i++) {
            uint64_t buf = page_alloc();  /* 分配一个 4KB 页面 */
            if (buf == 0) break;

            /* 从空闲链表取一个描述符 */
            uint16_t idx = vq->free_head;
            vq->free_head = vq->desc[idx].next;

            /* 设置描述符: 设备可写 (WRITE 标志)
             * 参考: [VIRTIO-SPEC] 2.6.5 "Descriptor Table" */
            vq->desc[idx].addr  = buf;
            vq->desc[idx].len   = PAGE_SIZE;
            vq->desc[idx].flags = VRING_DESC_F_WRITE;
            vq->desc[idx].next  = 0;

            /* 放入 available ring
             * 参考: [VIRTIO-SPEC] 2.6.6 "Available Ring" */
            vq->avail->ring[vq->avail->idx % vq->num] = idx;

            /* 内存屏障: 确保描述符写入在 idx 更新之前完成
             * 参考: [ARM-ARM] C6.2.4 "DMB, Data Memory Barrier" */
            __asm__ volatile("dmb sy" ::: "memory");

            vq->avail->idx++;
        }
        /* 通知设备: 接收队列有新缓冲区可用
         * 参考: [VIRTIO-SPEC] 2.6.13 "Notifying The Device" */
        vio_write16(VIRTIO_REG_QUEUE_NOTIFY, 0);
    }

    /* 步骤 10: 使能 GIC 中断 (用于接收通知) */
    gic_enable_irq(net_dev.pci->irq);

    /* 步骤 11: 设置 DRIVER_OK — 设备完全就绪
     * 参考: [VIRTIO-SPEC] 3.1 步骤 8 "Set the DRIVER_OK status bit" */
    vio_write8(VIRTIO_REG_DEVICE_STATUS,
               vio_read8(VIRTIO_REG_DEVICE_STATUS) | VIRTIO_STATUS_DRIVER_OK);

    uart_puts("[virtio-net] Device initialized and ready\n");
    return 0;
}

/* ==================================================================
 * virtio_net_send() — 发送一帧以太网数据
 *
 * 流程:
 *   1. 准备 virtio-net header (10 字节, 全零)
 *   2. 将 header + 数据打包到缓冲区
 *   3. 设置 TX descriptor
 *   4. 放入 available ring
 *   5. 通知设备 (写 QUEUE_NOTIFY)
 *
 * 参数:
 *   data — 以太网帧 (含以太网头)
 *   len  — 帧长度
 *
 * 参考: [VIRTIO-SPEC] 5.1.6.2 "Sending Packets"
 * ================================================================== */
int virtio_net_send(const void *data, uint32_t len)
{
    struct virtqueue *vq = &net_dev.tx_vq;

    /* virtio-net header 结构 (legacy: 10 字节)
     *
     * 参考: [VIRTIO-SPEC] 5.1.6 "Device Operation"
     *        struct virtio_net_hdr */
    struct {
        uint8_t  flags;         /* 0 = 无特殊标志 */
        uint8_t  gso_type;      /* 0 = VIRTIO_NET_HDR_GSO_NONE */
        uint16_t hdr_len;       /* 0 */
        uint16_t gso_size;      /* 0 */
        uint16_t csum_start;    /* 0 */
        uint16_t csum_offset;   /* 0 */
    } __attribute__((packed)) net_hdr;
    memset(&net_hdr, 0, sizeof(net_hdr));

    /* 分配发送缓冲区 (virtio-net header + 以太网帧) */
    uint64_t buf_phys = page_alloc();
    if (buf_phys == 0) {
        uart_puts("[virtio-net] TX: out of memory!\n");
        return -1;
    }
    uint8_t *buf = (uint8_t *)buf_phys;

    /* 复制 virtio-net header */
    memcpy(buf, &net_hdr, sizeof(net_hdr));
    /* 复制以太网帧数据 */
    memcpy(buf + sizeof(net_hdr), data, len);

    uint32_t total_len = sizeof(net_hdr) + len;

    /* 从空闲链表取一个描述符 */
    uint16_t idx = vq->free_head;
    vq->free_head = vq->desc[idx].next;

    /* 设置描述符: 设备只读 (无 WRITE 标志)
     * addr = 缓冲区物理地址
     * len  = 总长度 (header + 帧)
     * 参考: [VIRTIO-SPEC] 2.6.5 */
    vq->desc[idx].addr  = buf_phys;
    vq->desc[idx].len   = total_len;
    vq->desc[idx].flags = 0;             /* 设备只读 */
    vq->desc[idx].next  = 0;

    /* 放入 available ring
     * 参考: [VIRTIO-SPEC] 2.6.6 */
    vq->avail->ring[vq->avail->idx % vq->num] = idx;

    /* 内存屏障: 确保描述符和缓冲区写入完成
     * 参考: [ARM-ARM] C6.2.4 "DMB" */
    __asm__ volatile("dmb sy" ::: "memory");

    vq->avail->idx++;

    /* 通知设备: TX 队列有新数据
     * 写 QUEUE_NOTIFY = 1 (TX 队列号)
     * 这就是用户流程图中的 "Doorbell" — 通知网卡去发送!
     * 参考: [VIRTIO-SPEC] 2.6.13 "Notifying The Device" */
    vio_write16(VIRTIO_REG_QUEUE_NOTIFY, 1);

    return 0;
}

/* ==================================================================
 * virtio_net_recv() — 接收一帧以太网数据
 *
 * 检查 RX used ring 是否有新完成的接收缓冲区。
 * 如果有, 复制数据到用户提供的缓冲区。
 *
 * 参数:
 *   buf     — 接收缓冲区
 *   buf_len — 缓冲区大小
 *
 * 返回: 接收到的以太网帧字节数, 或 0 (无数据), 或 -1 (错误)
 *
 * 参考: [VIRTIO-SPEC] 5.1.6.3 "Receiving Packets"
 * ================================================================== */
int virtio_net_recv(void *buf, uint32_t buf_len)
{
    struct virtqueue *vq = &net_dev.rx_vq;

    /* 内存屏障: 确保读到最新的 used.idx
     * 参考: [ARM-ARM] C6.2.4 "DMB" */
    __asm__ volatile("dmb sy" ::: "memory");

    /* 检查是否有新的已完成接收
     * 比较 used.idx 和上次处理的位置 */
    if (vq->last_used_idx == vq->used->idx) {
        return 0;  /* 没有新数据 */
    }

    /* 取出已完成的 used ring 条目
     * 参考: [VIRTIO-SPEC] 2.6.8 "Used Ring" */
    struct virtq_used_elem *elem = &vq->used->ring[vq->last_used_idx % vq->num];
    uint16_t desc_idx = (uint16_t)elem->id;
    uint32_t total_len = elem->len;

    vq->last_used_idx++;

    /* 跳过 virtio-net header (10 字节), 只复制以太网帧 */
    uint32_t hdr_size = 10;
    uint32_t frame_len = 0;
    if (total_len > hdr_size) {
        frame_len = total_len - hdr_size;
        if (frame_len > buf_len)
            frame_len = buf_len;

        uint8_t *src = (uint8_t *)vq->desc[desc_idx].addr + hdr_size;
        memcpy(buf, src, frame_len);
    }

    /* 回收描述符: 重新放入 available ring 以供下次接收使用
     * 参考: [VIRTIO-SPEC] 5.1.6.3 "After receiving" */
    vq->desc[desc_idx].len   = PAGE_SIZE;
    vq->desc[desc_idx].flags = VRING_DESC_F_WRITE;

    vq->avail->ring[vq->avail->idx % vq->num] = desc_idx;
    __asm__ volatile("dmb sy" ::: "memory");
    vq->avail->idx++;

    /* 通知设备有新的接收缓冲区 */
    vio_write16(VIRTIO_REG_QUEUE_NOTIFY, 0);

    return (int)frame_len;
}

/* ==================================================================
 * virtio_net_get_mac() — 获取设备 MAC 地址
 * ================================================================== */
void virtio_net_get_mac(uint8_t *mac)
{
    memcpy(mac, net_dev.mac, 6);
}
