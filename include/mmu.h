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

/* ==================================================================
 * mmu_enable_user_access() — 为 RAM 区域启用 EL0 访问
 *
 * 在进入用户态 (EL0) 之前调用。
 * 修改 L1 页表 entry[1] 添加 AP[1]=1 (用户可访问),
 * 然后刷新 TLB 使新权限生效。
 *
 * 必须在 MMU 已启用后调用。
 * ================================================================== */
void mmu_enable_user_access(void);

#endif /* MMU_H */
