/*
 * include/pci.h — PCI/PCIe 总线枚举头文件
 *
 * PCIe (PCI Express) 是连接 CPU 和外设 (网卡、GPU 等) 的标准总线。
 * 本模块实现基本的 PCIe ECAM (Enhanced Configuration Access Mechanism) 枚举,
 * 扫描总线上的所有设备。
 *
 * 参考文档:
 *   [PCIE-SPEC] PCI Express Base Specification Revision 3.0
 *   [PCI-LPC]   PCI Local Bus Specification Revision 3.0
 *   [QEMU-VIRT] QEMU virt 平台: PCIe ECAM 基地址 = 0x3F00_0000
 */

#ifndef PCI_H
#define PCI_H

#include "types.h"

/* ==================================================================
 * PCIe 配置空间常量
 *
 * 每个 PCIe 设备有 4KB 的配置空间, 通过 ECAM 内存映射访问。
 * 配置空间地址 = ECAM_BASE + (bus << 20) + (dev << 15) + (func << 12) + reg
 *
 * 参考: [PCIE-SPEC] 7.2 "PCI Express Enhanced Configuration Access Mechanism"
 * ================================================================== */

/* ECAM 基地址 (QEMU virt 平台)
 * 参考: [QEMU-VIRT] hw/arm/virt.c VIRT_PCIE_ECAM = 0x3F00_0000 */
#define PCI_ECAM_BASE       0x3F000000UL

/* PCIe MMIO BAR 区域 (QEMU virt 平台)
 * 参考: [QEMU-VIRT] VIRT_PCIE_MMIO = 0x1000_0000, size = 0x2EFF_0000 */
#define PCI_MMIO_BASE       0x10000000UL
#define PCI_MMIO_SIZE       0x2EFF0000UL

/* PCIe 中断号 (QEMU virt SPI 起始)
 * 参考: [QEMU-VIRT] VIRT_PCIE_IRQ = 3, 映射到 SPI 32+3=35 */
#define PCI_IRQ_BASE        35

/* ==================================================================
 * PCI 配置空间寄存器偏移 (Type 0 Header)
 *
 * 参考: [PCI-LPC] 6.1 "Configuration Space Header"
 * ================================================================== */
#define PCI_VENDOR_ID       0x00    /* 16-bit: 厂商 ID */
#define PCI_DEVICE_ID       0x02    /* 16-bit: 设备 ID */
#define PCI_COMMAND         0x04    /* 16-bit: 命令寄存器 */
#define PCI_STATUS          0x06    /* 16-bit: 状态寄存器 */
#define PCI_CLASS_REVISION  0x08    /* 32-bit: [31:24]类[23:16]子类[15:8]接口[7:0]修订 */
#define PCI_BAR0            0x10    /* 32-bit: Base Address Register 0 */
#define PCI_BAR1            0x14    /* 32-bit: Base Address Register 1 */
#define PCI_INTERRUPT_LINE  0x3C    /* 8-bit: 中断线 */
#define PCI_INTERRUPT_PIN   0x3D    /* 8-bit: 中断引脚 */

/* PCI 命令寄存器位
 * 参考: [PCI-LPC] 6.2.2 "Command Register" */
#define PCI_CMD_IO          (1 << 0)   /* I/O 空间使能 */
#define PCI_CMD_MEMORY      (1 << 1)   /* 内存空间使能 */
#define PCI_CMD_BUS_MASTER  (1 << 2)   /* 总线主控使能 (DMA 需要) */

/* ==================================================================
 * PCI 设备描述结构
 * ================================================================== */
struct pci_device {
    uint8_t  bus;           /* 总线号 */
    uint8_t  dev;           /* 设备号 */
    uint8_t  func;          /* 功能号 */
    uint16_t vendor_id;     /* 厂商 ID */
    uint16_t device_id;     /* 设备 ID */
    uint8_t  class_code;    /* 类代码 */
    uint8_t  subclass;      /* 子类代码 */
    uint64_t bar0;          /* BAR0 映射的 MMIO 基地址 */
    uint32_t irq;           /* 分配的 GIC 中断号 */
};

/* ==================================================================
 * PCI 接口函数
 * ================================================================== */

/* pci_init() — 枚举 PCIe 总线上的所有设备 */
void pci_init(void);

/* pci_find_device() — 按厂商/设备 ID 查找设备
 * 返回: 找到的设备指针, 或 NULL */
struct pci_device *pci_find_device(uint16_t vendor_id, uint16_t device_id);

/* pci_config_read32() — 读取 PCIe 配置空间 32 位值 */
uint32_t pci_config_read32(uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset);

/* pci_config_write32() — 写入 PCIe 配置空间 32 位值 */
void pci_config_write32(uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset, uint32_t val);

/* pci_config_read16() — 读取 PCIe 配置空间 16 位值 */
uint16_t pci_config_read16(uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset);

/* pci_config_write16() — 写入 PCIe 配置空间 16 位值 */
void pci_config_write16(uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset, uint16_t val);

#endif /* PCI_H */
