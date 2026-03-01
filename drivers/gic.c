/*
 * drivers/gic.c — GICv2 中断控制器驱动
 *
 * GIC (Generic Interrupt Controller) v2 由两个主要组件构成:
 *   1. Distributor (GICD) — 全局中断路由器, 管理所有中断源的使能/优先级/目标
 *   2. CPU Interface (GICC) — 每核一个, 负责中断的确认和完成
 *
 * 中断处理流程:
 *   外设触发中断 → GICD 路由到目标核 → GICC 通知 CPU →
 *   CPU 读取 IAR 确认 → 处理中断 → 写 EOIR 完成
 *
 * 参考文档:
 *   [GIC-SPEC] ARM Generic Interrupt Controller Architecture Spec v2 (IHI 0048B)
 *   [QEMU-VIRT] QEMU virt 平台内存映射
 */

#include "gic.h"
#include "uart.h"

/* ==================================================================
 * GICv2 基地址 (QEMU virt 平台)
 *
 * 参考: [QEMU-VIRT] hw/arm/virt.c virt_memmap[]
 *   VIRT_GIC_DIST = 0x0800_0000
 *   VIRT_GIC_CPU  = 0x0801_0000
 * ================================================================== */
#define GICD_BASE   0x08000000UL    /* GIC Distributor 基地址 */
#define GICC_BASE   0x08010000UL    /* GIC CPU Interface 基地址 */

/* ==================================================================
 * GICD (Distributor) 寄存器偏移
 *
 * 参考: [GIC-SPEC] 4.3 "Distributor register map"
 * ================================================================== */

/* GICD_CTLR — 分发器控制寄存器 (偏移 0x000)
 * bit[0]: Enable — 1 启用分发器, 0 禁用
 * 参考: [GIC-SPEC] 4.3.1 "GICD_CTLR" */
#define GICD_CTLR       0x000

/* GICD_ISENABLER — 中断使能设置寄存器 (偏移 0x100-0x17C)
 * 每位对应一个中断号。写 1 = 使能, 写 0 = 无效果。
 * ISENABLER[n] 管理中断号 32n 到 32n+31。
 * 参考: [GIC-SPEC] 4.3.5 "GICD_ISENABLERn" */
#define GICD_ISENABLER  0x100

/* GICD_ICENABLER — 中断使能清除寄存器 (偏移 0x180-0x1FC)
 * 写 1 = 禁用对应中断。
 * 参考: [GIC-SPEC] 4.3.6 "GICD_ICENABLERn" */
#define GICD_ICENABLER  0x180

/* GICD_IPRIORITYR — 中断优先级寄存器 (偏移 0x400-0x7F8)
 * 每个中断占 8 位, 值越小优先级越高 (0 = 最高)。
 * 参考: [GIC-SPEC] 4.3.11 "GICD_IPRIORITYRn" */
#define GICD_IPRIORITYR 0x400

/* GICD_ITARGETSR — 中断目标 CPU 寄存器 (偏移 0x800-0xBF8)
 * 每个中断占 8 位, 每位对应一个 CPU (bit0=CPU0, bit1=CPU1, ...)。
 * 参考: [GIC-SPEC] 4.3.12 "GICD_ITARGETSRn" */
#define GICD_ITARGETSR  0x800

/* GICD_ICFGR — 中断配置寄存器 (偏移 0xC00-0xCFC)
 * 每个中断占 2 位: 0b01 = 电平触发, 0b11 = 边沿触发
 * 参考: [GIC-SPEC] 4.3.13 "GICD_ICFGRn" */
#define GICD_ICFGR      0xC00

/* ==================================================================
 * GICC (CPU Interface) 寄存器偏移
 *
 * 参考: [GIC-SPEC] 4.4 "CPU interface register map"
 * ================================================================== */

/* GICC_CTLR — CPU 接口控制寄存器 (偏移 0x000)
 * bit[0]: Enable — 1 启用 CPU 接口
 * 参考: [GIC-SPEC] 4.4.1 "GICC_CTLR" */
#define GICC_CTLR       0x000

/* GICC_PMR — 优先级屏蔽寄存器 (偏移 0x004)
 * 低于此优先级的中断将被屏蔽 (值越大 = 允许越多中断)。
 * 设为 0xFF 允许所有优先级的中断。
 * 参考: [GIC-SPEC] 4.4.2 "GICC_PMR" */
#define GICC_PMR        0x004

/* GICC_IAR — 中断确认寄存器 (偏移 0x00C)
 * 读取此寄存器: 返回当前最高优先级待处理中断号 + 确认该中断。
 * 如果没有待处理中断, 返回 1023 (伪中断)。
 * 参考: [GIC-SPEC] 4.4.4 "GICC_IAR" */
#define GICC_IAR        0x00C

/* GICC_EOIR — 中断结束寄存器 (偏移 0x010)
 * 写入中断号: 通知 GIC 该中断处理完毕, 可以重新触发。
 * 参考: [GIC-SPEC] 4.4.5 "GICC_EOIR" */
#define GICC_EOIR       0x010

/* ==================================================================
 * MMIO 读写宏
 *
 * volatile 确保编译器不会优化掉对硬件寄存器的读写。
 * ================================================================== */
#define REG32(base, off) (*(volatile uint32_t *)((base) + (off)))

/* ==================================================================
 * gic_init() — 初始化 GICv2
 *
 * 步骤:
 *   1. 禁用 Distributor 和 CPU Interface
 *   2. 配置优先级屏蔽 (允许所有中断)
 *   3. 启用 Distributor 和 CPU Interface
 *
 * 参考: [GIC-SPEC] 4.3.1, 4.4.1
 * ================================================================== */
void gic_init(void)
{
    uart_puts("[gic] Initializing GICv2...\n");

    /* 步骤 1: 禁用 Distributor
     * 在修改配置前先禁用, 防止意外中断。
     * 参考: [GIC-SPEC] 4.3.1 "GICD_CTLR — Enable=0" */
    REG32(GICD_BASE, GICD_CTLR) = 0;

    /* 步骤 2: 禁用 CPU Interface
     * 参考: [GIC-SPEC] 4.4.1 "GICC_CTLR — Enable=0" */
    REG32(GICC_BASE, GICC_CTLR) = 0;

    /* 步骤 3: 设置优先级屏蔽为最低
     * PMR = 0xFF → 允许所有优先级 (0-255) 的中断通过
     * 参考: [GIC-SPEC] 4.4.2 "GICC_PMR" */
    REG32(GICC_BASE, GICC_PMR) = 0xFF;

    /* 步骤 4: 启用 Distributor
     * CTLR.Enable = 1 → 开始分发中断
     * 参考: [GIC-SPEC] 4.3.1 */
    REG32(GICD_BASE, GICD_CTLR) = 1;

    /* 步骤 5: 启用 CPU Interface
     * CTLR.Enable = 1 → CPU 开始接收中断
     * 参考: [GIC-SPEC] 4.4.1 */
    REG32(GICC_BASE, GICC_CTLR) = 1;

    uart_puts("[gic] GICv2 initialized (Dist=0x08000000, CPU=0x08010000)\n");
}

/* ==================================================================
 * gic_enable_irq() — 使能指定中断号
 *
 * GICD_ISENABLER 是一组 32 位寄存器, 每位对应一个中断号。
 * 寄存器号 = irq / 32, 位号 = irq % 32。
 *
 * 参数:
 *   irq — GIC 中断号 (0-1019)
 *
 * 参考: [GIC-SPEC] 4.3.5 "GICD_ISENABLERn"
 * ================================================================== */
void gic_enable_irq(uint32_t irq)
{
    /* 计算寄存器偏移: ISENABLER[irq/32] = ISENABLER_base + (irq/32)*4 */
    uint32_t reg = GICD_ISENABLER + (irq / 32) * 4;

    /* 计算位偏移: 第 irq%32 位 */
    uint32_t bit = 1U << (irq % 32);

    /* 写入 1 使能该中断 (写 0 无效果)
     * 参考: [GIC-SPEC] "GICD_ISENABLERn — 写 1 使能, 写 0 无效" */
    REG32(GICD_BASE, reg) = bit;

    /* 设置中断优先级为 0xA0 (中等优先级)
     * 每个中断占 IPRIORITYR 中的 1 字节
     * 参考: [GIC-SPEC] 4.3.11 */
    volatile uint8_t *prio = (volatile uint8_t *)(GICD_BASE + GICD_IPRIORITYR + irq);
    *prio = 0xA0;

    /* 设置中断目标为 CPU0 (bit 0)
     * 每个中断占 ITARGETSR 中的 1 字节
     * 参考: [GIC-SPEC] 4.3.12 */
    volatile uint8_t *target = (volatile uint8_t *)(GICD_BASE + GICD_ITARGETSR + irq);
    *target = 0x01;         /* 路由到 CPU0 */
}

/* ==================================================================
 * gic_read_iar() — 读取中断确认寄存器
 *
 * 返回当前最高优先级的待处理中断号, 同时自动确认该中断。
 * 返回 1023 表示没有待处理中断 (伪中断)。
 *
 * 参考: [GIC-SPEC] 4.4.4 "GICC_IAR"
 * ================================================================== */
uint32_t gic_read_iar(void)
{
    return REG32(GICC_BASE, GICC_IAR);
}

/* ==================================================================
 * gic_write_eoir() — 写入中断结束寄存器
 *
 * 通知 GIC 该中断已处理完毕。
 * 必须在中断处理完成后调用, 否则 GIC 不会再次触发该中断。
 *
 * 参考: [GIC-SPEC] 4.4.5 "GICC_EOIR"
 * ================================================================== */
void gic_write_eoir(uint32_t irq)
{
    REG32(GICC_BASE, GICC_EOIR) = irq;
}
