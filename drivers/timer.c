/*
 * drivers/timer.c — ARM 通用定时器驱动
 *
 * ARM64 的 Generic Timer 提供一个精确的系统时钟源,
 * 可配置为在固定间隔后触发 PPI 中断。
 *
 * 使用的是 Non-secure Physical Timer (EL1):
 *   - CNTPCT_EL0: 物理计数器 (单调递增, 只读)
 *   - CNTP_TVAL_EL0: 定时器值 (倒计数, 到 0 触发中断)
 *   - CNTP_CTL_EL0: 定时器控制寄存器
 *   - CNTFRQ_EL0: 计数器频率 (Hz)
 *
 * 参考文档:
 *   [ARM-ARM] D11 "The Generic Timer in AArch64 state"
 *   [ARM-ARM] D11.2 "Timer registers"
 */

#include "timer.h"
#include "gic.h"
#include "uart.h"
#include "types.h"

/* ==================================================================
 * 定时器间隔
 *
 * 我们设置定时器每秒触发一次中断 (1 Hz)。
 * 间隔 = 计数器频率 × 秒数
 * QEMU virt 平台默认频率: 62.5 MHz (0x3B9ACA0)。
 *
 * 参考: [ARM-ARM] D11.1.2 "The counter frequency"
 * ================================================================== */
static uint64_t timer_interval;

/* 滴答计数器 (每次定时器中断 +1, 用于调试) */
static uint64_t tick_count = 0;

/* ==================================================================
 * timer_init() — 初始化 ARM 通用定时器
 *
 * 步骤:
 *   1. 读取计数器频率 (CNTFRQ_EL0)
 *   2. 在 GIC 中使能定时器中断 (IRQ 30)
 *   3. 设置倒计时值 (CNTP_TVAL_EL0)
 *   4. 使能定时器 (CNTP_CTL_EL0)
 *
 * 参考: [ARM-ARM] D11.2 "Timer registers"
 * ================================================================== */
void timer_init(void)
{
    uart_puts("[timer] Initializing ARM Generic Timer...\n");

    /* 步骤 1: 读取计数器频率
     *
     * CNTFRQ_EL0 保存计数器的时钟频率 (单位: Hz)。
     * 此值由固件/引导加载器在启动时设置。
     * QEMU virt 默认为 62500000 (62.5 MHz)。
     *
     * 参考: [ARM-ARM] D13.11.2 "CNTFRQ_EL0, Counter-timer Frequency" */
    uint64_t freq;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));

    /* 设置定时器间隔为 1 秒 (freq 个计数 = 1 秒)
     * 如果频率太高可以用 freq/10 来获得 100ms 间隔 */
    timer_interval = freq;  /* 1 秒 */

    uart_puts("[timer] Counter frequency: ");
    /* 简单打印频率 */
    char buf[11];
    int i = 10;
    buf[i] = '\0';
    uint64_t v = freq;
    while (v > 0) {
        buf[--i] = '0' + (v % 10);
        v /= 10;
    }
    uart_puts(&buf[i]);
    uart_puts(" Hz\n");

    /* 步骤 2: 在 GIC 中使能定时器中断
     *
     * Non-secure physical timer 使用 PPI #14 (GIC IRQ 30)。
     * 参考: [ARM-ARM] D11.2.4 "Timer interrupt generation"
     *        [GIC-SPEC] "PPI IDs" */
    gic_enable_irq(TIMER_IRQ);

    /* 步骤 3: 设置定时器倒计时值
     *
     * CNTP_TVAL_EL0 (Counter-timer Physical Timer TimerValue):
     * 写入一个值 N 后, 定时器从 N 开始递减, 到 0 时触发中断。
     *
     * 参考: [ARM-ARM] D13.11.21 "CNTP_TVAL_EL0" */
    __asm__ volatile("msr cntp_tval_el0, %0" :: "r"(timer_interval));

    /* 步骤 4: 使能定时器
     *
     * CNTP_CTL_EL0 (Counter-timer Physical Timer Control):
     *   bit[0] ENABLE = 1 → 启动定时器
     *   bit[1] IMASK  = 0 → 不屏蔽中断输出
     *   bit[2] ISTATUS (只读) → 1 表示条件满足
     *
     * 参考: [ARM-ARM] D13.11.20 "CNTP_CTL_EL0" */
    __asm__ volatile("msr cntp_ctl_el0, %0" :: "r"(1UL));

    uart_puts("[timer] Timer started (1 second interval)\n");
}

/* ==================================================================
 * timer_handler() — 定时器中断处理函数
 *
 * 由 IRQ 异常处理中调用 (当 GIC IAR 返回 TIMER_IRQ 时)。
 *
 * 步骤:
 *   1. 增加滴答计数
 *   2. 重设定时器倒计时 (否则中断只触发一次)
 *
 * 参考: [ARM-ARM] D11.2.4 "Timer interrupt generation"
 * ================================================================== */
void timer_handler(void)
{
    tick_count++;

    /* 每 5 秒打印一次心跳信息 (避免刷屏) */
    if (tick_count % 5 == 0) {
        uart_puts("[timer] tick=");
        char buf[11];
        int i = 10;
        buf[i] = '\0';
        uint64_t v = tick_count;
        if (v == 0) buf[--i] = '0';
        while (v > 0) {
            buf[--i] = '0' + (v % 10);
            v /= 10;
        }
        uart_puts(&buf[i]);
        uart_puts("\n");
    }

    /* 重设定时器倒计时值
     * 必须在每次中断后重新写入 TVAL, 否则只触发一次。
     * 参考: [ARM-ARM] D13.11.21 "CNTP_TVAL_EL0" */
    __asm__ volatile("msr cntp_tval_el0, %0" :: "r"(timer_interval));
}
