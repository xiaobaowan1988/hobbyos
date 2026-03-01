/*
 * kernel/main.c — 内核 C 语言入口
 *
 * 这是从汇编启动代码 (boot/start.S) 跳转过来的第一个 C 函数。
 * 在这里进行各子系统的初始化, 然后进入内核主循环。
 *
 * 参考文档:
 *   [ARM-ARM] ARM Architecture Reference Manual ARMv8-A (DDI 0487)
 *   [QEMU-VIRT] https://www.qemu.org/docs/master/system/arm/virt.html
 */

#include "uart.h"
#include "mm.h"
#include "mmu.h"
#include "gic.h"
#include "timer.h"
#include "proc.h"

/* ==================================================================
 * kernel_main() — 内核主函数
 *
 * 由 boot/start.S 中的 `bl kernel_main` 调用。
 * 此时 CPU 运行在 EL1 (内核态), 栈已设置好, BSS 已清零。
 *
 * 当前章节 (Chapter 01) 仅初始化 UART 并打印启动信息。
 * 后续章节将在此函数中逐步添加更多子系统的初始化。
 * ================================================================== */
void kernel_main(void)
{
    /* 步骤 1: 初始化 UART — 这是最基础的输出设备,
     * 所有后续调试信息都依赖它。 */
    uart_init();

    /* 步骤 2: 打印启动横幅 — 确认系统已成功启动到 C 环境 */
    uart_puts("=========================================\n");
    uart_puts("  HobbyOS — ARM64 bare-metal kernel\n");
    uart_puts("  Chapter 01: Boot + UART\n");
    uart_puts("=========================================\n");
    uart_puts("\n");

    /* 步骤 3: 打印基本系统信息 */
    uart_puts("[boot] CPU is running at EL1 (kernel mode)\n");
    uart_puts("[boot] UART0 (PL011) initialized at 0x09000000\n");
    uart_puts("[boot] BSS section zeroed\n");
    uart_puts("[boot] Stack pointer set\n");
    uart_puts("\n");
    uart_puts("[boot] Exception vector table installed (VBAR_EL1)\n");
    uart_puts("\n");

    /* 步骤 3: 初始化物理内存管理器 */
    pmm_init();
    uart_puts("\n");

    /* 步骤 4: 初始化 MMU (恒等映射 + 缓存) */
    mmu_init();
    uart_puts("\n");

    /* 步骤 5: 初始化 GIC 中断控制器 */
    gic_init();
    uart_puts("\n");

    /* 步骤 6: 初始化 ARM 通用定时器 */
    timer_init();
    uart_puts("\n");

    /* 步骤 7: 初始化进程子系统 */
    proc_init();
    uart_puts("\n");

    /* 步骤 6: 创建用户态测试进程并调度运行 */
    uart_puts("[boot] Creating user process...\n");
    extern void user_entry(void);       /* user/user_prog.c 中定义 */
    proc_create(user_entry);

    uart_puts("[boot] Scheduling first user process...\n\n");
    schedule();

    /* 如果 schedule 返回, 表示无更多进程可运行 */
    uart_puts("\n[boot] All processes exited. Halting.\n");
    while (1) {
        __asm__ volatile("wfe");
    }
}
