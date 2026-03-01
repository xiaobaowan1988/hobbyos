/*
 * drivers/pci.c — PCIe ECAM 总线枚举与配置
 *
 * PCIe 使用 ECAM (Enhanced Configuration Access Mechanism) 将每个设备的
 * 配置空间映射到一段连续的 MMIO 地址空间。
 *
 * ECAM 地址计算:
 *   config_addr = ECAM_BASE + (bus << 20) | (dev << 15) | (func << 12) | reg
 *
 * 枚举流程: 遍历所有 bus/dev/func 组合, 读取 Vendor ID,
 * 如果不是 0xFFFF 则表示该位置有设备。
 *
 * 参考文档:
 *   [PCIE-SPEC] PCI Express Base Specification Revision 3.0
 *   [PCI-LPC]   PCI Local Bus Specification Revision 3.0
 *   [QEMU-VIRT] QEMU virt 平台内存映射
 */

#include "pci.h"
#include "uart.h"

/* ==================================================================
 * 已发现设备列表
 * ================================================================== */
#define MAX_PCI_DEVICES 32
static struct pci_device pci_devices[MAX_PCI_DEVICES];
static int pci_device_count = 0;

/* 用于 BAR 分配的当前 MMIO 地址 (简单递增分配) */
static uint64_t pci_mmio_next = PCI_MMIO_BASE;

/* ==================================================================
 * ecam_addr() — 计算 ECAM 配置空间地址
 *
 * ECAM 地址编码:
 *   bits[27:20] = bus    (8 位, 最多 256 个总线)
 *   bits[19:15] = device (5 位, 最多 32 个设备/总线)
 *   bits[14:12] = function (3 位, 最多 8 个功能/设备)
 *   bits[11:0]  = register offset (12 位, 4KB 配置空间)
 *
 * 参考: [PCIE-SPEC] 7.2.2 "ECAM address calculation"
 * ================================================================== */
static volatile void *ecam_addr(uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset)
{
    uint64_t addr = PCI_ECAM_BASE
                  | ((uint64_t)bus  << 20)   /* 总线号 */
                  | ((uint64_t)dev  << 15)   /* 设备号 */
                  | ((uint64_t)func << 12)   /* 功能号 */
                  | (uint64_t)offset;         /* 寄存器偏移 */
    return (volatile void *)addr;
}

/* ==================================================================
 * PCIe 配置空间读写函数
 *
 * 通过 ECAM MMIO 直接读写配置空间寄存器。
 * volatile 确保每次都真正访问硬件。
 *
 * 参考: [PCIE-SPEC] 7.2 "Configuration Space Access"
 * ================================================================== */

/* 读取 32 位值 */
uint32_t pci_config_read32(uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset)
{
    return *(volatile uint32_t *)ecam_addr(bus, dev, func, offset);
}

/* 写入 32 位值 */
void pci_config_write32(uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset, uint32_t val)
{
    *(volatile uint32_t *)ecam_addr(bus, dev, func, offset) = val;
}

/* 读取 16 位值 */
uint16_t pci_config_read16(uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset)
{
    return *(volatile uint16_t *)ecam_addr(bus, dev, func, offset);
}

/* 写入 16 位值 */
void pci_config_write16(uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset, uint16_t val)
{
    *(volatile uint16_t *)ecam_addr(bus, dev, func, offset) = val;
}

/* ==================================================================
 * 辅助函数: 打印十六进制
 * ================================================================== */
static void print_hex16(uint16_t val)
{
    const char hex[] = "0123456789abcdef";
    char buf[5];
    buf[0] = hex[(val >> 12) & 0xF];
    buf[1] = hex[(val >> 8) & 0xF];
    buf[2] = hex[(val >> 4) & 0xF];
    buf[3] = hex[val & 0xF];
    buf[4] = '\0';
    uart_puts(buf);
}

static void print_hex32(uint32_t val)
{
    print_hex16((uint16_t)(val >> 16));
    print_hex16((uint16_t)(val & 0xFFFF));
}

/* ==================================================================
 * pci_probe_device() — 探测并配置一个 PCIe 设备
 *
 * 步骤:
 *   1. 读取 Vendor/Device ID
 *   2. 读取 Class/Subclass
 *   3. 配置 BAR0 (分配 MMIO 地址)
 *   4. 使能 Memory Space + Bus Master
 *   5. 记录设备信息
 *
 * 参考: [PCI-LPC] 6.1 "Configuration Space Header"
 * ================================================================== */
static void pci_probe_device(uint8_t bus, uint8_t dev, uint8_t func)
{
    /* 读取 Vendor ID (偏移 0x00, 低 16 位)
     * Vendor ID = 0xFFFF 表示该位置没有设备。
     * 参考: [PCI-LPC] 6.2.1 "Vendor ID" */
    uint16_t vendor = pci_config_read16(bus, dev, func, PCI_VENDOR_ID);
    if (vendor == 0xFFFF)
        return;  /* 没有设备 */

    /* 读取 Device ID (偏移 0x02) */
    uint16_t device = pci_config_read16(bus, dev, func, PCI_DEVICE_ID);

    /* 读取 Class/Subclass (偏移 0x08)
     * bits[31:24] = Class Code (大类)
     * bits[23:16] = Subclass (子类)
     * 参考: [PCI-LPC] Appendix D "Class Codes" */
    uint32_t class_rev = pci_config_read32(bus, dev, func, PCI_CLASS_REVISION);
    uint8_t class_code = (class_rev >> 24) & 0xFF;
    uint8_t subclass   = (class_rev >> 16) & 0xFF;

    /* 打印设备信息 */
    uart_puts("[pci] ");
    uart_putc('0' + bus);
    uart_puts(":");
    uart_putc('0' + dev);
    uart_puts(".");
    uart_putc('0' + func);
    uart_puts("  vendor=0x");
    print_hex16(vendor);
    uart_puts(" device=0x");
    print_hex16(device);
    uart_puts(" class=0x");
    print_hex16((uint16_t)((class_code << 8) | subclass));
    uart_puts("\n");

    /* 配置 BAR0: 探测 BAR 大小并分配 MMIO 地址
     *
     * BAR 大小探测方法 (标准 PCI 方法):
     *   1. 保存 BAR 原始值
     *   2. 写入 0xFFFFFFFF
     *   3. 读回 → 低位为 0 的位数表示对齐要求 (即大小)
     *   4. 恢复原始值 (或写入新地址)
     *
     * 参考: [PCI-LPC] 6.2.5 "Base Address Registers" */
    uint32_t bar0_orig = pci_config_read32(bus, dev, func, PCI_BAR0);

    /* 写全 1 探测大小 */
    pci_config_write32(bus, dev, func, PCI_BAR0, 0xFFFFFFFF);
    uint32_t bar0_size_mask = pci_config_read32(bus, dev, func, PCI_BAR0);

    /* 恢复原始值 */
    pci_config_write32(bus, dev, func, PCI_BAR0, bar0_orig);

    uint64_t bar0_addr = 0;
    if (bar0_size_mask != 0 && bar0_size_mask != 0xFFFFFFFF) {
        /* 计算 BAR 大小
         * 对于 Memory BAR: 低 4 位是类型标志, 需要屏蔽
         * size = ~(mask & ~0xF) + 1
         * 参考: [PCI-LPC] 6.2.5.1 "Memory Base Address Registers" */
        uint32_t size = ~(bar0_size_mask & ~0xFU) + 1;

        /* 对齐 MMIO 分配地址 */
        pci_mmio_next = (pci_mmio_next + size - 1) & ~((uint64_t)size - 1);
        bar0_addr = pci_mmio_next;
        pci_mmio_next += size;

        /* 写入分配的 MMIO 地址到 BAR0 */
        pci_config_write32(bus, dev, func, PCI_BAR0, (uint32_t)bar0_addr);

        uart_puts("[pci]   BAR0 = 0x");
        print_hex32((uint32_t)bar0_addr);
        uart_puts(" size=0x");
        print_hex32(size);
        uart_puts("\n");
    }

    /* 使能 Memory Space 和 Bus Master
     *
     * Bus Master 使能后, 设备可以发起 DMA 传输 (主动读写内存)。
     * 这对网卡至关重要 — 网卡需要 DMA 来读取发送缓冲区。
     *
     * 参考: [PCI-LPC] 6.2.2 "Command Register" */
    uint16_t cmd = pci_config_read16(bus, dev, func, PCI_COMMAND);
    cmd |= PCI_CMD_MEMORY | PCI_CMD_BUS_MASTER;
    pci_config_write16(bus, dev, func, PCI_COMMAND, cmd);

    /* 记录设备到列表 */
    if (pci_device_count < MAX_PCI_DEVICES) {
        struct pci_device *p = &pci_devices[pci_device_count++];
        p->bus = bus;
        p->dev = dev;
        p->func = func;
        p->vendor_id = vendor;
        p->device_id = device;
        p->class_code = class_code;
        p->subclass = subclass;
        p->bar0 = bar0_addr;
        /* QEMU virt PCIe 中断映射: SPI (32 + 3 + dev%4)
         * 参考: [QEMU-VIRT] virt_pcie_irqmap */
        p->irq = PCI_IRQ_BASE + (dev % 4);
    }
}

/* ==================================================================
 * pci_init() — 枚举 PCIe 总线
 *
 * 扫描 bus 0 的所有 device/function 组合。
 * (QEMU virt 只有一条总线, 无需递归扫描桥设备)
 *
 * 参考: [PCI-LPC] 6.2 "Device identification"
 * ================================================================== */
void pci_init(void)
{
    uart_puts("[pci] Scanning PCIe bus (ECAM @ 0x3F000000)...\n");

    /* 遍历 bus 0 的所有 32 个设备槽, 每个最多 8 个功能
     *
     * 真实系统需要递归扫描 PCI-to-PCI 桥 (class 0x06/0x04),
     * 但 QEMU virt 只有一条总线, 简化处理即可。
     *
     * 参考: [PCI-LPC] 6.4 "Configuration Transactions" */
    for (uint8_t dev = 0; dev < 32; dev++) {
        for (uint8_t func = 0; func < 8; func++) {
            pci_probe_device(0, dev, func);
        }
    }

    uart_puts("[pci] Found ");
    uart_putc('0' + pci_device_count);
    uart_puts(" device(s)\n");
}

/* ==================================================================
 * pci_find_device() — 按 Vendor/Device ID 查找设备
 *
 * 参数:
 *   vendor_id — 要查找的厂商 ID
 *   device_id — 要查找的设备 ID
 *
 * 返回:
 *   找到: 指向 pci_device 结构的指针
 *   未找到: NULL
 * ================================================================== */
struct pci_device *pci_find_device(uint16_t vendor_id, uint16_t device_id)
{
    for (int i = 0; i < pci_device_count; i++) {
        if (pci_devices[i].vendor_id == vendor_id &&
            pci_devices[i].device_id == device_id)
            return &pci_devices[i];
    }
    return NULL;
}
