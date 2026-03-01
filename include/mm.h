/*
 * include/mm.h — 内存管理头文件
 *
 * 定义页面分配器的接口和内存相关常量。
 *
 * 参考文档:
 *   [ARM-ARM] ARM Architecture Reference Manual ARMv8-A (DDI 0487)
 *   [ARM-ARM] D5.2 "The VMSAv8-64 translation table format"
 */

#ifndef MM_H
#define MM_H

#include "types.h"

/* ==================================================================
 * 内存常量
 * ================================================================== */

/* PAGE_SHIFT — 页面大小的位移量
 * ARM64 支持 4KB、16KB、64KB 页面。我们使用最常见的 4KB。
 * 4KB = 2^12, 所以 PAGE_SHIFT = 12。
 * 参考: [ARM-ARM] D5.2.1 "Supported granule sizes" */
#define PAGE_SHIFT          12

/* PAGE_SIZE — 页面大小 (4096 字节 = 4KB)
 * 参考: [ARM-ARM] D5.2.1 "4KB granule" */
#define PAGE_SIZE           (1UL << PAGE_SHIFT)

/* PAGE_MASK — 页面对齐掩码
 * 用于将地址向下对齐到页面边界: addr & PAGE_MASK
 * 例: 0x40001234 & ~0xFFF = 0x40001000 */
#define PAGE_MASK           (~(PAGE_SIZE - 1))

/* QEMU virt 平台 RAM 布局:
 * 参考: [QEMU-VIRT] hw/arm/virt.c MemMapEntry virt_memmap[]
 *
 * RAM 起始: 0x4000_0000
 * 默认 RAM 大小: 由 -m 参数指定 (我们使用 512MB)
 * RAM 结束: 0x4000_0000 + 512MB = 0x6000_0000 */
#define MEMORY_BASE         0x40000000UL
#define MEMORY_SIZE         (512UL * 1024 * 1024)   /* 512 MB */
#define MEMORY_END          (MEMORY_BASE + MEMORY_SIZE)

/* ==================================================================
 * 页面分配器接口
 * ================================================================== */

/* pmm_init() — 初始化物理页面分配器
 *
 * 扫描从内核映像结束地址 (_end) 到 RAM 末尾的可用物理内存,
 * 建立空闲页面链表。
 *
 * 此函数必须在使用 page_alloc/page_free 之前调用。 */
void pmm_init(void);

/* page_alloc() — 分配一个物理页面 (4KB)
 *
 * 返回:
 *   成功: 页面的物理地址 (4KB 对齐)
 *   失败: 0 (无可用页面)
 *
 * 注意: 返回的页面内容未清零。如需清零, 调用者自行处理。 */
uint64_t page_alloc(void);

/* page_free() — 释放一个物理页面
 *
 * 参数:
 *   addr — 要释放的页面物理地址 (必须是 4KB 对齐且之前由 page_alloc 分配) */
void page_free(uint64_t addr);

#endif /* MM_H */
