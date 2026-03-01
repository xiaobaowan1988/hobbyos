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
 * RAM 区域使用 L1→L2→L3 三级映射:
 *   L1 entry[1] → L2 table (将 1GB RAM 分为 512 个 2MB 块)
 *   L2 entry[0] → L3 table (将首 2MB 分为 512 个 4KB 页面)
 *   L2 entry[1..255] → 2MB 块描述符 (剩余 RAM)
 *
 * 权限分离:
 *   代码页 (read-only):  AP[2:1]=0b11, UXN=0 → EL1+EL0 均可执行
 *   数据页 (read-write): AP[2:1]=0b01, UXN=1 → EL1+EL0 可读写, 仅 EL1 可执行
 *
 * ARMv8 安全规则 (get_S1prot):
 *   在 EL1&0 翻译体制中, 如果 UXN=0 且 EL0 可写, EL1 不可执行。
 *   即: EL1 执行条件 = NOT(PXN) AND (UXN OR NOT(writable_at_EL0))
 *   因此代码页必须对 EL0 只读, 数据页必须设 UXN=1。
 *
 * 参考文档:
 *   [ARM-ARM] D5 "The AArch64 Virtual Memory System Architecture"
 *   [ARM-ARM] D5.2 "VMSAv8-64 translation table format"
 *   [ARM-ARM] D5.3 "VMSAv8-64 translation table descriptor formats"
 *   [ARM-ARM] D5.4.5 "Access permissions"
 *   [ARM-ARM] D5.4.6 "Execute-never controls"
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

/* 访问权限 (AP) — bits[7:6]
 *
 * AP[2:1] 控制 EL1 和 EL0 的读写权限:
 *   AP[2:1] | EL1       | EL0
 *   0b00    | Read/Write | No access
 *   0b01    | Read/Write | Read/Write
 *   0b10    | Read-only  | No access
 *   0b11    | Read-only  | Read-only
 *
 * 参考: [ARM-ARM] D5.4.5 "Access permissions" Table D5-34 */
#define PTE_USER        (1UL << 6)   /* AP[1]=1 → EL0 可访问 */
#define PTE_RO          (1UL << 7)   /* AP[2]=1 → 只读 */

/* 不可执行 (XN) — 用于数据页面, 防止代码注入攻击
 * UXN (bit[54]): User eXecute Never
 * PXN (bit[53]): Privileged eXecute Never
 *
 * 重要: ARMv8 安全规则规定, 在 EL1&0 翻译体制中:
 *   EL1 执行条件 = NOT(PXN) AND (UXN OR NOT(writable_at_EL0))
 *
 * 这意味着: 如果 EL0 可写 (AP[2:1]=0b01) 且 UXN=0, 则 EL1 不可执行!
 * 解决方法:
 *   - 代码页: AP[2:1]=0b11 (只读), UXN=0 → EL1 可执行 (NOT(writable) = true)
 *   - 数据页: AP[2:1]=0b01 (读写), UXN=1 → EL1 可执行 (UXN = true)
 *
 * 参考: [ARM-ARM] D5.4.6 "Execute-never controls" */
#define PTE_UXN         (1UL << 54)
#define PTE_PXN         (1UL << 53)

/* ==================================================================
 * 页表结构
 *
 * L1 → L2 → L3 三级页表覆盖 RAM 区域:
 *
 *   page_table_l1[0]  = 1GB 块 (设备内存 0x00000000-0x3FFFFFFF)
 *   page_table_l1[1]  = Table → page_table_l2_ram
 *     page_table_l2_ram[0]    = Table → page_table_l3_code
 *       page_table_l3_code[0..7]   = 4KB 页 (代码, 只读+可执行)
 *       page_table_l3_code[8..511] = 4KB 页 (数据, 读写+UXN)
 *     page_table_l2_ram[1..255]  = 2MB 块 (数据, 读写+UXN)
 *     page_table_l2_ram[256..511] = 无效 (超出 RAM)
 *
 * 参考: [ARM-ARM] D5.2.4 "Translation table walks"
 * ================================================================== */
static uint64_t page_table_l1[512] __attribute__((aligned(4096)));
static uint64_t page_table_l2_ram[512] __attribute__((aligned(4096)));
static uint64_t page_table_l3_code[512] __attribute__((aligned(4096)));

/* ==================================================================
 * mmu_init() — 初始化 MMU
 *
 * 步骤:
 *   1. 配置 MAIR_EL1 (内存属性寄存器)
 *   2. 配置 TCR_EL1 (翻译控制寄存器)
 *   3. 填充三级页表 (L1→L2→L3, 恒等映射)
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
     *   T0SZ  [5:0]   = 25  → 有效地址空间 = 2^(64-25) = 2^39 = 512GB
     *                         T0SZ=25 使翻译从 Level 1 开始 (4KB 粒度下,
     *                         T0SZ∈[25,33] 对应 Level 1 起始)。
     *                         若 T0SZ=16, 翻译从 Level 0 开始,
     *                         而 Level 0 不支持 Block 描述符!
     *   IRGN0 [9:8]   = 0b01 → Inner Write-Back, Read-Allocate, Write-Allocate
     *   ORGN0 [11:10]  = 0b01 → Outer Write-Back, Read-Allocate, Write-Allocate
     *   SH0   [13:12]  = 0b11 → Inner Shareable
     *   TG0   [15:14]  = 0b00 → 4KB granule (TTBR0_EL1)
     *   IPS   [34:32]  = 0b000 → 32-bit physical (4GB, 足够 QEMU 512MB)
     *
     * 参考: [ARM-ARM] D13.2.120 "TCR_EL1"
     *        [ARM-ARM] D5.2.6 Table D5-11 "Initial lookup level by T0SZ"
     * ============================================================== */
    uint64_t tcr = (25UL << 0)       /* T0SZ = 25 → 39-bit VA, Level 1 start */
                 | (1UL << 8)        /* IRGN0 = WB RA WA */
                 | (1UL << 10)       /* ORGN0 = WB RA WA */
                 | (3UL << 12)       /* SH0 = Inner Shareable */
                 | (0UL << 14);      /* TG0 = 4KB granule */
    __asm__ volatile("msr tcr_el1, %0" :: "r"(tcr));

    /* ==============================================================
     * 步骤 3: 填充三级页表 — 恒等映射 (VA == PA)
     *
     * 内存布局:
     *   0x0000_0000 - 0x3FFF_FFFF (1GB): 设备内存 (L1 block)
     *   0x4000_0000 - 0x7FFF_FFFF (1GB): RAM (L1→L2→L3)
     *
     * RAM 权限分区:
     *   0x4000_0000 - 0x4000_7FFF (32KB): 代码+只读数据
     *     → AP[2:1]=0b11 (只读), UXN=0 → EL0+EL1 可执行
     *   0x4000_8000 - 0x5FFF_FFFF (511MB): 数据+BSS+堆
     *     → AP[2:1]=0b01 (读写), UXN=1 → EL0+EL1 可读写
     *     → EL1 可执行 (UXN=1 满足安全规则), EL0 不可执行
     *
     * 参考: [ARM-ARM] D5.3.1 各级描述符格式
     *        [QEMU-VIRT] hw/arm/virt.c virt_memmap[]
     * ============================================================== */

    /* 先将所有页表清零 (所有条目无效) */
    for (int i = 0; i < 512; i++) {
        page_table_l1[i] = 0;
        page_table_l2_ram[i] = 0;
        page_table_l3_code[i] = 0;
    }

    /* L1 Entry 0: 设备内存区域 (0x0000_0000 - 0x3FFF_FFFF)
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

    /* ----------------------------------------------------------
     * 填充 L3 页表: 将首 2MB (0x4000_0000 - 0x401F_FFFF) 分为 4KB 页
     *
     * 代码区域 (pages 0-7, 0x40000000-0x40007FFF):
     *   包含 .text.boot, .text, .rodata — 只读, 可执行
     *   AP[2:1]=0b11: EL0+EL1 只读
     *   UXN=0: EL0 可执行 (用户代码 user_entry 在此区域)
     *   PXN=0: EL1 可执行 (内核代码在此区域)
     *   由于 EL0 不可写, 满足 ARM 安全规则:
     *     EL1 执行 = NOT(PXN) AND (UXN OR NOT(writable_EL0))
     *             = true AND (false OR true) = true ✓
     *
     * 数据区域 (pages 8-511, 0x40008000-0x401FFFFF):
     *   包含 .data, .bss, 启动栈, 页表 — 读写, 不可执行
     *   AP[2:1]=0b01: EL0+EL1 读写
     *   UXN=1: EL0 不可执行 (防止用户态代码注入)
     *   PXN=0: EL1 可执行 (安全规则: UXN=1 → 条件满足)
     *
     * 参考: [ARM-ARM] D5.3.2 "Level 3 descriptor format"
     * ---------------------------------------------------------- */

    /* .data 段起始页索引 — 由链接器符号 __data_start 动态计算
     * 这样即使代码大小变化导致段重排, 权限分界也能自动适应 */
    extern char __data_start[];
    uint64_t data_start_page = ((uint64_t)__data_start - 0x40000000UL) / PAGE_SIZE;

    for (int i = 0; i < 512; i++) {
        uint64_t pa = 0x40000000UL + (uint64_t)i * PAGE_SIZE;

        if ((uint64_t)i < data_start_page) {
            /* 代码页: 只读 + 可执行 (EL0 和 EL1) */
            page_table_l3_code[i] = pa
                | PTE_VALID | PTE_PAGE      /* L3 用 PTE_PAGE (bit[1]=1) */
                | PTE_AF
                | PTE_ATTR_NORMAL | PTE_ISH
                | PTE_USER | PTE_RO;        /* AP[2:1]=0b11: 只读 */
                                            /* UXN=0, PXN=0: 都可执行 */
        } else {
            /* 数据页: 读写 + 不可执行(EL0) */
            page_table_l3_code[i] = pa
                | PTE_VALID | PTE_PAGE
                | PTE_AF
                | PTE_ATTR_NORMAL | PTE_ISH
                | PTE_USER                  /* AP[2:1]=0b01: 读写 */
                | PTE_UXN;                  /* UXN=1: EL0 不可执行 */
        }
    }

    /* L2 Entry 0: 指向 L3 页表 (首 2MB 使用 4KB 页面粒度) */
    page_table_l2_ram[0] = (uint64_t)page_table_l3_code
                         | PTE_VALID
                         | PTE_TABLE;

    /* ----------------------------------------------------------
     * 填充 L2 entries 1-255: 2MB 块描述符
     *
     * 覆盖 RAM 的剩余部分 (0x4020_0000 - 0x5FFF_FFFF)。
     * 这些区域用于物理页面分配器管理的内存。
     * 权限: EL0+EL1 读写, EL0 不可执行, EL1 可执行。
     *
     * 参考: [ARM-ARM] D5.3.1 "Level 2 Block descriptor format"
     * ---------------------------------------------------------- */
    for (int i = 1; i < 256; i++) {
        page_table_l2_ram[i] = (0x40000000UL + (uint64_t)i * 0x200000UL)
            | PTE_VALID | PTE_BLOCK
            | PTE_AF
            | PTE_ATTR_NORMAL | PTE_ISH
            | PTE_USER                      /* AP[2:1]=0b01: EL0+EL1 读写 */
            | PTE_UXN;                      /* UXN=1: EL0 不可执行 */
    }
    /* L2 entries 256-511: 无效 (0x60000000-0x7FFFFFFF 超出 512MB RAM) */

    /* L1 Entry 1: RAM 区域 → L2 页表 */
    page_table_l1[1] = (uint64_t)page_table_l2_ram
                     | PTE_VALID
                     | PTE_TABLE;

    uart_puts("[mmu] L1→L2→L3 page table: code=RO+exec, data=RW+UXN\n");

    /* ==============================================================
     * 步骤 4: 设置 TTBR0_EL1 (Translation Table Base Register 0)
     *
     * TTBR0_EL1 保存页表的基地址, 起始级别由 T0SZ 决定。
     * 由于 T0SZ=25, 翻译从 Level 1 开始, TTBR0 直接指向 Level 1 表。
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
    /* 读取当前 SCTLR_EL1 (保留 RES1 位) 并修改需要的位。
     * SCTLR_EL1 包含多个 RES1 位 (bits 11, 20, 22, 23, 28, 29),
     * 这些位必须保持为 1, 否则行为 UNPREDICTABLE。
     *
     * 参考: [ARM-ARM] D13.2.112 "SCTLR_EL1" */
    uint64_t sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));

    sctlr |= (1UL << 0);           /* M = 1: 使能 MMU */
    sctlr |= (1UL << 2);           /* C = 1: 使能 D-Cache */
    sctlr |= (1UL << 12);          /* I = 1: 使能 I-Cache */
    sctlr &= ~(1UL << 19);         /* WXN = 0: 可写区域仍可执行
                                     * 参考: [ARM-ARM] D13.2.112 bit[19] */

    __asm__ volatile(
        "msr sctlr_el1, %0\n"     /* 写入 SCTLR_EL1, 立即生效 */
        "isb\n"                    /* 指令同步屏障: 确保新设置生效 */
        :: "r"(sctlr)
    );

    uart_puts("[mmu] MMU enabled (identity mapping, D-Cache + I-Cache ON)\n");
}

/* ==================================================================
 * mmu_enable_user_access() — 用户访问已在 mmu_init 中配置
 *
 * 在当前实现中, 页表在 mmu_init() 中已设置好 EL0 权限:
 *   - 代码页: AP[2:1]=0b11 (只读) → EL0 可读+执行
 *   - 数据页: AP[2:1]=0b01 (读写) + UXN=1 → EL0 可读写
 *
 * 此函数保留为接口兼容, 仅打印确认信息。
 *
 * 参考: [ARM-ARM] D5.4.5 "Access permissions" Table D5-34
 * ================================================================== */
void mmu_enable_user_access(void)
{
    uart_puts("[mmu] User access enabled for RAM region\n");
}
