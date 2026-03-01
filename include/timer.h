/*
 * include/timer.h — ARM 通用定时器头文件
 *
 * ARM64 内置 Generic Timer, 通过系统寄存器直接访问 (无需 MMIO)。
 *
 * 参考文档:
 *   [ARM-ARM] D11 "The Generic Timer in AArch64 state"
 */

#ifndef TIMER_H
#define TIMER_H

/* timer_init() — 初始化并启动定时器 */
void timer_init(void);

/* timer_handler() — 定时器中断处理函数 */
void timer_handler(void);

#endif /* TIMER_H */
