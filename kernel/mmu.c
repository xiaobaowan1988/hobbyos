/*
 * kernel/mmu.c — MMU 初始化与页表管理
 *
 * 配置 ARM64 的 MMU (Memory Management Unit), 建立内核页表,
 * 实现虚拟地址到物理地址的映射。
 *
 * 本章使用恒等映射 (Identity Mapping): VA == PA,
 * 即虚拟地址等于物理地址。这是最简单的方案,
 * 开启 MMU 前后代码可以无缝继续执行。
 *
 * ARMv8-A 使用 4 级页表 (Level 0-3), 4KB 粒度时:
 *   Level 0: 每条目覆盖 512GB (bits[47:39])
 *   Level 1: 每条目覆盖 1GB   (bits[38:30])  — 可直接映射 1GB 块
 *   Level 2: 每条目覆盖 2MB   (bits[29:21])  — 可直接映射 2MB 块
 *   Level 3: 每条目覆盖 4KB   (bits[20:12])  — 最终 4KB 页面
 *
 * 本章为简化, 使用 Level 1 的 1GB 块映射 (Block Descriptor),
 * 足以覆盖整个 RAM 和设备 MMIO 区域。
 *
 * 参考文档:
 *   [ARM-ARM] D5 "The AArch64 Virtual Memory System Architecture"
 *   [ARM-ARM] D5.2 "VMSAv8-64 translation table format"
 *   [ARM-ARM] D5.3 "VMSAv8-64 translation table descriptor formats"
 *   [ARM-ARM] D13.2 "AArch64 System Register descriptions"
 */

#include "types.h"
#include "mm.h"
#include "uart.h"

/* ==================================================================
 * 页表描述符中的属性位定义
 *
 * 参考: [ARM-ARM] D5.3.1 "VMSAv8-64 translation table level 0, 1, 2 descriptor formats"
 *        [ARM-ARM] D5.3.3 "Memory attribute fields in the VMSAv8-64 translation table format"
 * ================================================================== */

/* 描述符有效位 — bit[0]
 * 0 = 无效 (访问时触发 Translation Fault)
 * 1 = 有效
 * 参考: [ARM-ARM] D5.3.1 Table D5-15 */
#define PTE_VALID       (1UL << 0)

/* 描述符类型 — bit[1]
 * Level 0/1/2:
 *   0 = Block descriptor (直接映射大块内存)
 *   1 = Table descriptor (指向下一级页表)
 * Level 3:
 *   1 = Page descriptor (最终 4KB 页面)
 *
 * 参考: [ARM-ARM] D5.3.1 Table D5-15 */
#define PTE_TABLE       (1UL << 1)
#define PTE_BLOCK       (0UL << 1)   /* Level 1/2 块描述符 */
#define PTE_PAGE        (1UL << 1)   /* Level 3 页描述符 */

/* 访问标志 (AF) — bit[10]
 * 首次访问时若 AF=0, CPU 会触发 Access Flag Fault。
 * 我们预设为 1 以避免此异常。
 * 参考: [ARM-ARM] D5.4.4 "Access flag" */
#define PTE_AF          (1UL << 10)

/* 共享性 (SH) — bits[9:8]
 * 0b00 = Non-shareable
 * 0b10 = Outer Shareable
 * 0b11 = Inner Shareable (多核一致性需要此设置)
 * 参考: [ARM-ARM] D5.5 "Memory attribute, Shareability" */
#define PTE_ISH         (3UL << 8)   /* Inner Shareable */
#define PTE_OSH         (2UL << 8)   /* Outer Shareable */

/* 属性索引 (AttrIndx) — bits[4:2]
 * 索引到 MAIR_EL1 寄存器中的 8 个属性槽位。
 * 我们定义:
 *   Index 0 = Device-nGnRnE (设备内存, 无缓存, 无重排序)
 *   Index 1 = Normal, Write-Back Cacheable (普通可缓存内存)
 *
 * 参考: [ARM-ARM] D5.5 "Memory types and attributes"
 *        [ARM-ARM] D13.2.82 "MAIR_EL1" */
#define PTE_ATTR_DEVICE (0UL << 2)   /* AttrIndx = 0 → 设备内存 */
#define PTE_ATTR_NORMAL (1UL << 2)   /* AttrIndx = 1 → 普通内存 */

/* EL0 可访问 — AP[1], bit[6]
 * 0 = 仅 EL1 可访问
 * 1 = EL0 和 EL1 均可访问
 * 参考: [ARM-ARM] D5.4.5 "Access permissions" Table D5-34 */
#define PTE_USER        (1UL << 6)

/* 不可执行 (XN) — 用于数据页面, 防止代码注入攻击
 * UXN (bit[54]): User eXecute Never
 * PXN (bit[53]): Privileged eXecute Never
 * 参考: [ARM-ARM] D5.4.6 "Execute-never controls" */
#define PTE_UXN         (1UL << 54)
#define PTE_PXN         (1UL << 53)

/* ==================================================================
 * 内核页表 (Level 1)
 *
 * 使用 1GB 块描述符 (Level 1 Block Descriptor)。
 * Level 1 页表有 512 个条目, 每个条目映射 1GB。
 * 512 × 1GB = 512GB, 覆盖整个低地址空间。
 *
 * 页表必须 4KB 对齐 (4096 字节)。
 * 使用 GCC __attribute__((aligned(4096))) 确保对齐。
 *
 * 参考: [ARM-ARM] D5.2.4 "Translation table walks"
 * ================================================================== */
static uint64_t page_table_l1[512] __attribute__((aligned(4096)));

/* ==================================================================
 * mmu_init() — 初始化 MMU
 *
 * 步骤:
 *   1. 配置 MAIR_EL1 (内存属性寄存器)
 *   2. 配置 TCR_EL1 (翻译控制寄存器)
 *   3. 填充 Level 1 页表 (恒等映射)
 *   4. 设置 TTBR0_EL1 (页表基地址)
 *   5. 使能 MMU (写 SCTLR_EL1)
 *
 * 参考: [ARM-ARM] D13.2 "System register descriptions"
 * ================================================================== */
void mmu_init(void)
{
    uart_puts("[mmu] Initializing MMU with identity mapping...\n");

    /* ==============================================================
     * 步骤 1: 配置 MAIR_EL1 (Memory Attribute Indirection Register)
     *
     * MAIR_EL1 定义 8 个内存属性槽位 (每个 8 位)。
     * 页表描述符中的 AttrIndx 字段索引到此寄存器。
     *
     * 我们配置两个槽位:
     *   Attr0 = 0x00: Device-nGnRnE (设备内存)
     *     - 无缓存、无 Gathering、无 Reordering、无 Early Write Ack
     *     - 用于 UART、GIC、PCIe 等硬件寄存器
     *
     *   Attr1 = 0xFF: Normal, Write-Back, Read/Write Allocate
     *     - Inner: Write-Back, Read-Allocate, Write-Allocate (0b1111)
     *     - Outer: Write-Back, Read-Allocate, Write-Allocate (0b1111)
     *     - 用于 RAM (代码、数据、栈、堆)
     *
     * 参考: [ARM-ARM] D13.2.82 "MAIR_EL1"
     *        [ARM-ARM] D5.5.1 "Memory types" Table D5-45
     * ============================================================== */
    uint64_t mair = (0x00UL << 0)    /* Attr0 = Device-nGnRnE */
                  | (0xFFUL << 8);   /* Attr1 = Normal WB RAWA */
    __asm__ volatile("msr mair_el1, %0" :: "r"(mair));

    /* ==============================================================
     * 步骤 2: 配置 TCR_EL1 (Translation Control Register)
     *
     * TCR_EL1 控制地址翻译的各种参数。
     *
     * 关键字段:
     *   T0SZ  [5:0]   = 16  → 有效地址空间 = 2^(64-16) = 2^48 = 256TB
     *   IRGN0 [9:8]   = 0b01 → Inner Write-Back, Read-Allocate, Write-Allocate
     *   ORGN0 [11:10]  = 0b01 → Outer Write-Back, Read-Allocate, Write-Allocate
     *   SH0   [13:12]  = 0b11 → Inner Shareable
     *   TG0   [15:14]  = 0b00 → 4KB granule (TTBR0_EL1)
     *   IPS   [34:32]  = 0b000 → 32-bit physical (4GB, 足够 QEMU 512MB)
     *
     * 参考: [ARM-ARM] D13.2.120 "TCR_EL1"
     * ============================================================== */
    uint64_t tcr = (16UL << 0)       /* T0SZ = 16 → 48-bit VA */
                 | (1UL << 8)        /* IRGN0 = WB RA WA */
                 | (1UL << 10)       /* ORGN0 = WB RA WA */
                 | (3UL << 12)       /* SH0 = Inner Shareable */
                 | (0UL << 14);      /* TG0 = 4KB granule */
    __asm__ volatile("msr tcr_el1, %0" :: "r"(tcr));

    /* ==============================================================
     * 步骤 3: 填充 Level 1 页表 — 恒等映射 (VA == PA)
     *
     * Level 1 每个条目映射 1GB。我们映射前 4 个条目 (0-4GB):
     *
     *   Entry 0: 0x0000_0000 - 0x3FFF_FFFF (1GB)
     *     → 设备内存 (UART 0x0900_0000, GIC, etc.)
     *     → 属性: Device-nGnRnE, 不可缓存
     *
     *   Entry 1: 0x4000_0000 - 0x7FFF_FFFF (1GB)
     *     → RAM (包含内核代码和数据)
     *     → 属性: Normal, Write-Back Cacheable
     *
     *   Entry 2-3: 可选, 用于扩展设备映射
     *
     * 参考: [ARM-ARM] D5.3.1 "Level 1 Block descriptor format"
     *        [QEMU-VIRT] hw/arm/virt.c virt_memmap[]
     * ============================================================== */

    /* 先将整个页表清零 (所有条目无效) */
    for (int i = 0; i < 512; i++)
        page_table_l1[i] = 0;

    /* Entry 0: 设备内存区域 (0x0000_0000 - 0x3FFF_FFFF)
     * 包含:
     *   - UART0:    0x0900_0000
     *   - RTC:      0x0901_0000
     *   - GIC Dist: 0x0800_0000
     *   - GIC CPU:  0x0801_0000
     *   - PCIe ECAM: 0x3F00_0000
     *   - 等等
     *
     * 使用 Device-nGnRnE 属性, 确保 MMIO 访问不被缓存或重排序。
     * 参考: [ARM-ARM] D5.5 "Device memory attributes" */
    page_table_l1[0] = (0x00000000UL)       /* 物理基地址 = 0 */
                     | PTE_VALID             /* 有效 */
                     | PTE_BLOCK             /* 1GB 块描述符 */
                     | PTE_AF                /* Access Flag = 1 */
                     | PTE_ATTR_DEVICE       /* AttrIndx = 0 (Device) */
                     | PTE_OSH              /* Outer Shareable (设备) */
                     | PTE_UXN               /* User 不可执行 */
                     | PTE_PXN;              /* Privileged 不可执行 */

    /* Entry 1: RAM 区域 (0x4000_0000 - 0x7FFF_FFFF)
     * 包含内核代码、数据、栈、物理页面等。
     * 使用 Normal Cacheable 属性以获得最佳性能。
     * 参考: [ARM-ARM] D5.5 "Normal memory attributes" */
    page_table_l1[1] = (0x40000000UL)       /* 物理基地址 = 1GB */
                     | PTE_VALID             /* 有效 */
                     | PTE_BLOCK             /* 1GB 块描述符 */
                     | PTE_AF                /* Access Flag = 1 */
                     | PTE_ATTR_NORMAL       /* AttrIndx = 1 (Normal) */
                     | PTE_ISH;              /* Inner Shareable (RAM) */

    uart_puts("[mmu] L1 page table: entry[0]=Device 0-1GB, entry[1]=Normal 1-2GB\n");

    /* ==============================================================
     * 步骤 4: 设置 TTBR0_EL1 (Translation Table Base Register 0)
     *
     * TTBR0_EL1 保存 Level 0 (或 Level 1, 取决于 T0SZ) 页表的基地址。
     * 由于 T0SZ=16, 实际上 TTBR0 指向 Level 1 表 (跳过 Level 0)。
     *
     * 参考: [ARM-ARM] D13.2.132 "TTBR0_EL1"
     * ============================================================== */
    uint64_t ttbr0 = (uint64_t)page_table_l1;
    __asm__ volatile("msr ttbr0_el1, %0" :: "r"(ttbr0));

    /* ==============================================================
     * 步骤 5: 指令和数据屏障
     *
     * 在开启 MMU 之前, 必须确保:
     *   - 所有之前的内存写入 (包括页表) 都已完成 (DSB)
     *   - 所有旧的地址翻译缓存 (TLB) 被清除 (TLBI)
     *   - 指令流水线已刷新 (ISB)
     *
     * 参考: [ARM-ARM] D5.9 "TLB maintenance"
     *        [ARM-ARM] C6.2.4  "DSB, Data Synchronization Barrier"
     *        [ARM-ARM] C6.2.8  "ISB, Instruction Synchronization Barrier"
     * ============================================================== */
    __asm__ volatile(
        "dsb sy\n"                 /* 数据同步屏障: 等待所有内存操作完成 */
        "isb\n"                    /* 指令同步屏障: 刷新流水线 */
        "tlbi vmalle1\n"           /* 清除 EL1 的所有 TLB 条目 */
        "dsb sy\n"                 /* 等待 TLB 清除完成 */
        "isb\n"                    /* 刷新流水线 */
    );

    /* ==============================================================
     * 步骤 6: 使能 MMU — 写 SCTLR_EL1 (System Control Register)
     *
     * SCTLR_EL1 关键位:
     *   M   (bit[0])  = 1 → 使能 MMU
     *   C   (bit[2])  = 1 → 使能数据缓存 (D-Cache)
     *   I   (bit[12]) = 1 → 使能指令缓存 (I-Cache)
     *
     * 注意: 由于我们使用恒等映射 (VA == PA),
     * 开启 MMU 后 PC 不需要跳转, 代码可以直接继续执行。
     *
     * 参考: [ARM-ARM] D13.2.112 "SCTLR_EL1"
     * ============================================================== */
    uint64_t sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));

    sctlr |= (1UL << 0);           /* M = 1: 使能 MMU */
    sctlr |= (1UL << 2);           /* C = 1: 使能 D-Cache */
    sctlr |= (1UL << 12);          /* I = 1: 使能 I-Cache */

    __asm__ volatile(
        "msr sctlr_el1, %0\n"     /* 写入 SCTLR_EL1, 立即生效 */
        "isb\n"                    /* 指令同步屏障: 确保新设置生效 */
        :: "r"(sctlr)
    );

    uart_puts("[mmu] MMU enabled (identity mapping, D-Cache + I-Cache ON)\n");
}
