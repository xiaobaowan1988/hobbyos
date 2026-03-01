/*
 * kernel/pmm.c — 物理内存管理器 (Physical Memory Manager)
 *
 * 使用空闲链表 (free list) 算法管理物理页面:
 *   - 每个空闲页面的前 8 字节存储下一个空闲页面的地址
 *   - free_list_head 指向链表头
 *   - 分配 = 从链表头取出一个页面
 *   - 释放 = 将页面插入链表头
 *
 * 这是最简单的页面分配器实现, 分配和释放都是 O(1) 操作。
 * Linux 内核使用更复杂的伙伴系统 (Buddy System), 但原理类似。
 *
 * 参考文档:
 *   [OSTEP] Operating Systems: Three Easy Pieces, Chapter 17 "Free-Space Management"
 *   [QEMU-VIRT] QEMU virt 平台内存布局
 */

#include "mm.h"
#include "uart.h"

/* ==================================================================
 * 空闲链表节点结构
 *
 * 每个空闲页面的开头存储一个 free_page 结构,
 * 其中 next 指向下一个空闲页面。
 *
 * 当页面被分配后, 这个结构会被调用者的数据覆盖,
 * 因此不会浪费额外内存 — 这是经典的"侵入式链表"设计。
 *
 * 参考: Linux 内核 mm/page_alloc.c 中的 free_area / page 结构
 * ================================================================== */
struct free_page {
    struct free_page *next;         /* 指向下一个空闲页面 (或 NULL) */
};

/* ==================================================================
 * 全局变量
 * ================================================================== */

/* free_list_head — 空闲页面链表头指针
 * 初始为 NULL (在 pmm_init 之前没有可分配的页面)。 */
static struct free_page *free_list_head = NULL;

/* free_page_count — 当前空闲页面数量 (用于调试和统计) */
static uint64_t free_page_count = 0;

/* ==================================================================
 * 外部符号: _end (由链接脚本 linker.ld 定义)
 *
 * _end 标记内核映像 (代码 + 数据 + BSS) 的结束地址。
 * 可用于分配的物理内存从 _end 之后开始。
 *
 * 参考: scripts/linker.ld 中 "_end = .;" 定义
 * ================================================================== */
extern char _end[];

/* ==================================================================
 * 辅助函数: 打印十六进制数 (简单版, 用于启动信息)
 * ================================================================== */
static void print_hex(uint64_t val)
{
    const char hex[] = "0123456789abcdef";
    char buf[17];
    for (int i = 15; i >= 0; i--) {
        buf[i] = hex[val & 0xF];
        val >>= 4;
    }
    buf[16] = '\0';
    uart_puts("0x");
    uart_puts(buf);
}

/* ==================================================================
 * 辅助函数: 打印十进制数
 * ================================================================== */
static void print_dec(uint64_t val)
{
    char buf[21];               /* 64 位最大 20 位十进制 + '\0' */
    int i = 20;
    buf[i] = '\0';
    if (val == 0) {
        uart_putc('0');
        return;
    }
    while (val > 0) {
        buf[--i] = '0' + (val % 10);
        val /= 10;
    }
    uart_puts(&buf[i]);
}

/* ==================================================================
 * pmm_init() — 初始化物理页面分配器
 *
 * 步骤:
 *   1. 计算可用内存的起始地址 (内核映像结束后, 页对齐)
 *   2. 将每个空闲页面链入空闲链表
 *
 * 内存布局:
 *   0x4000_0000 ─────── RAM 起始 (= MEMORY_BASE)
 *       │  内核映像 (.text, .rodata, .data, .bss)
 *   _end ────────────── 内核映像结束
 *       │  ↓ 可分配的物理页面从这里开始 (向上对齐到 PAGE_SIZE)
 *       │  page 0, page 1, page 2, ...
 *   0x6000_0000 ─────── RAM 结束 (= MEMORY_END, 512MB)
 *
 * 参考: [OSTEP] Chapter 17 "Free-Space Management"
 * ================================================================== */
void pmm_init(void)
{
    uart_puts("[pmm] Initializing physical memory manager...\n");

    /* 步骤 1: 计算可用内存起始地址
     *
     * (uint64_t)_end — 内核映像结束的物理地址
     * + PAGE_SIZE - 1 — 向上对齐 (round up)
     * & PAGE_MASK     — 清除低 12 位, 得到页对齐地址
     *
     * 例: _end = 0x4000_5678
     *     + 0xFFF = 0x4001_5677
     *     & ~0xFFF = 0x4001_5000 */
    uint64_t start = ((uint64_t)_end + PAGE_SIZE - 1) & PAGE_MASK;

    /* 步骤 2: 确定可用内存结束地址 */
    uint64_t end = MEMORY_END;

    /* 打印内存范围信息 */
    uart_puts("[pmm] Kernel ends at:   ");
    print_hex((uint64_t)_end);
    uart_puts("\n");
    uart_puts("[pmm] Free memory:      ");
    print_hex(start);
    uart_puts(" - ");
    print_hex(end);
    uart_puts("\n");

    /* 步骤 3: 将所有空闲页面链入空闲链表
     *
     * 从 start 到 end, 每 PAGE_SIZE (4KB) 一个页面,
     * 将其插入空闲链表头部。
     *
     * 每个空闲页面的前 8 字节用作 free_page.next 指针,
     * 指向链表中的下一个空闲页面。 */
    free_list_head = NULL;
    free_page_count = 0;

    for (uint64_t addr = start; addr + PAGE_SIZE <= end; addr += PAGE_SIZE) {
        /* 将当前页面地址转为 free_page 结构指针
         * 直接在物理地址上操作 (此时尚未开启 MMU) */
        struct free_page *page = (struct free_page *)addr;

        /* 头插法: 新页面的 next 指向当前链表头 */
        page->next = free_list_head;

        /* 更新链表头为当前页面 */
        free_list_head = page;

        free_page_count++;
    }

    /* 打印统计信息 */
    uart_puts("[pmm] Free pages:       ");
    print_dec(free_page_count);
    uart_puts(" (");
    print_dec(free_page_count * PAGE_SIZE / 1024 / 1024);
    uart_puts(" MB)\n");
}

/* ==================================================================
 * page_alloc() — 分配一个物理页面
 *
 * 从空闲链表头部取出一个页面。
 *
 * 时间复杂度: O(1)
 *
 * 返回:
 *   成功: 页面的物理地址 (4KB 对齐)
 *   失败: 0 (没有空闲页面)
 *
 * 注意: 当前实现没有锁保护, 不支持多核并发分配。
 *       后续章节添加自旋锁后会修复。
 *
 * 参考: Linux 内核 mm/page_alloc.c __alloc_pages()
 * ================================================================== */
uint64_t page_alloc(void)
{
    /* 检查空闲链表是否为空 */
    if (free_list_head == NULL) {
        uart_puts("[pmm] ERROR: Out of memory!\n");
        return 0;                   /* 无可用页面, 返回 0 表示失败 */
    }

    /* 从链表头取出一个页面 */
    struct free_page *page = free_list_head;

    /* 更新链表头为下一个空闲页面 */
    free_list_head = page->next;

    /* 更新空闲页面计数 */
    free_page_count--;

    /* 返回页面的物理地址
     * 将指针转为 uint64_t 整数 (物理地址) */
    return (uint64_t)page;
}

/* ==================================================================
 * page_free() — 释放一个物理页面
 *
 * 将页面插入空闲链表头部。
 *
 * 时间复杂度: O(1)
 *
 * 参数:
 *   addr — 要释放的页面物理地址 (必须 4KB 对齐)
 *
 * 注意: 调用者有责任确保:
 *   1. addr 是之前由 page_alloc() 分配的
 *   2. 不要重复释放同一个页面 (double-free)
 *   3. addr 是 4KB 对齐的
 *
 * 参考: Linux 内核 mm/page_alloc.c __free_pages()
 * ================================================================== */
void page_free(uint64_t addr)
{
    /* 基本安全检查: 地址必须页对齐且在有效范围内 */
    if (addr == 0 || (addr & (PAGE_SIZE - 1)) != 0) {
        uart_puts("[pmm] ERROR: Invalid page_free address!\n");
        return;
    }

    /* 将物理地址转为 free_page 结构指针 */
    struct free_page *page = (struct free_page *)addr;

    /* 头插法: 将页面插入空闲链表头部 */
    page->next = free_list_head;
    free_list_head = page;

    /* 更新空闲页面计数 */
    free_page_count++;
}
