/*
 * include/gic.h — GICv2 中断控制器头文件
 *
 * GIC (Generic Interrupt Controller) 是 ARM 标准的中断控制器。
 * QEMU virt 平台使用 GICv2。
 *
 * 参考文档:
 *   [GIC-SPEC] ARM Generic Interrupt Controller Architecture Specification v2 (IHI 0048B)
 *   [QEMU-VIRT] QEMU virt 平台: GIC Distributor 0x0800_0000, CPU Interface 0x0801_0000
 */

#ifndef GIC_H
#define GIC_H

#include "types.h"

/* ==================================================================
 * GICv2 中断号定义
 *
 * ARM GIC 的中断号分布:
 *   0-15:   SGI (Software Generated Interrupts) — 核间中断
 *   16-31:  PPI (Private Peripheral Interrupts) — 每核私有外设中断
 *   32-1019: SPI (Shared Peripheral Interrupts) — 共享外设中断
 *
 * 参考: [GIC-SPEC] 1.4.2 "Interrupt types"
 * ================================================================== */

/* ARM 通用定时器 PPI 中断号
 * Non-secure physical timer = PPI #14 → GIC 中断号 = 16 + 14 = 30
 * 参考: [ARM-ARM] D11.2 "Timer interrupts" */
#define TIMER_IRQ       30

/* ==================================================================
 * GIC 接口函数
 * ================================================================== */

/* gic_init() — 初始化 GICv2 (Distributor + CPU Interface) */
void gic_init(void);

/* gic_enable_irq() — 使能指定中断号
 * 参数: irq — GIC 中断号 (0-1019) */
void gic_enable_irq(uint32_t irq);

/* gic_read_iar() — 读取 IAR (Interrupt Acknowledge Register)
 * 返回: 当前触发的中断号 (或 1023 表示伪中断) */
uint32_t gic_read_iar(void);

/* gic_write_eoir() — 写入 EOIR (End of Interrupt Register)
 * 参数: irq — 要结束的中断号 */
void gic_write_eoir(uint32_t irq);

#endif /* GIC_H */
