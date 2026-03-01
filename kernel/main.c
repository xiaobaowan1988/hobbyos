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
    uart_puts("[boot] Kernel boot complete. Halting.\n");

    /* 步骤 4: 内核主循环 — 当前无事可做, 进入无限循环。
     * 使用 WFE (Wait For Event) 指令让 CPU 进入低功耗状态,
     * 减少不必要的电力消耗。
     * 参考: [ARM-ARM] C6.2.33 "WFE, Wait For Event" */
    while (1) {
        /* ARM64 内联汇编: 执行 WFE 指令
         * __asm__ — GCC 内联汇编关键字
         * volatile — 防止编译器优化掉此指令
         * 参考: GCC 手册 "6.47 How to Use Inline Assembly Language in C Code" */
        __asm__ volatile("wfe");
    }
}
