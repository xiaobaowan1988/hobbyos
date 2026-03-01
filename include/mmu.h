/*
 * include/mmu.h — MMU 管理头文件
 *
 * 参考文档:
 *   [ARM-ARM] D5 "The AArch64 Virtual Memory System Architecture"
 */

#ifndef MMU_H
#define MMU_H

/* ==================================================================
 * mmu_init() — 初始化 MMU
 *
 * 建立恒等映射 (VA == PA) 页表并使能 MMU 和缓存。
 * 必须在 pmm_init() 之后调用 (需要页表内存)。
 * ================================================================== */
void mmu_init(void);

#endif /* MMU_H */
