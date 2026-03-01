/*
 * drivers/uart.c — PL011 UART 驱动 (轮询模式)
 *
 * PL011 是 ARM 标准的串口控制器, QEMU virt 平台默认提供。
 * 本驱动仅实现最基础的字符输出, 用于启动早期的调试打印。
 *
 * 参考文档:
 *   [PL011-TRM] ARM PrimeCell UART (PL011) Technical Reference Manual (DDI 0183)
 *   [QEMU-VIRT] QEMU virt 平台: UART0 基地址 = 0x0900_0000
 */

#include "uart.h"

/* ==================================================================
 * PL011 寄存器偏移量定义
 *
 * PL011 的寄存器以 MMIO 方式映射到物理地址空间。
 * 每个寄存器占 4 字节 (32-bit), 以下为关键寄存器的偏移。
 *
 * 参考: [PL011-TRM] Table 3-1 "PL011 Register Summary"
 * ================================================================== */

/* UARTDR — 数据寄存器 (偏移 0x000)
 * 写入: 将字符放入发送 FIFO
 * 读取: 从接收 FIFO 取出字符
 * 参考: [PL011-TRM] 3.3.1 "Data Register, UARTDR" */
#define UART_DR     0x000

/* UARTFR — 标志寄存器 (偏移 0x018)
 * 只读, 包含 FIFO 状态标志位。
 * 参考: [PL011-TRM] 3.3.3 "Flag Register, UARTFR" */
#define UART_FR     0x018

/* UARTIBRD — 整数波特率除数寄存器 (偏移 0x024)
 * 参考: [PL011-TRM] 3.3.6 "Integer Baud Rate Register, UARTIBRD" */
#define UART_IBRD   0x024

/* UARTFBRD — 小数波特率除数寄存器 (偏移 0x028)
 * 参考: [PL011-TRM] 3.3.7 "Fractional Baud Rate Register, UARTFBRD" */
#define UART_FBRD   0x028

/* UARTLCR_H — 线控制寄存器 (偏移 0x02C)
 * 配置数据位数、停止位、奇偶校验、FIFO 启用等。
 * 参考: [PL011-TRM] 3.3.8 "Line Control Register, UARTLCR_H" */
#define UART_LCR_H  0x02C

/* UARTCR — 控制寄存器 (偏移 0x030)
 * 控制 UART 的使能、发送使能、接收使能等。
 * 参考: [PL011-TRM] 3.3.9 "Control Register, UARTCR" */
#define UART_CR     0x030

/* UARTIMSC — 中断屏蔽设置/清除寄存器 (偏移 0x038)
 * 参考: [PL011-TRM] 3.3.11 "Interrupt Mask Set/Clear Register, UARTIMSC" */
#define UART_IMSC   0x038

/* ==================================================================
 * UARTFR 标志位定义
 * 参考: [PL011-TRM] 3.3.3 "Flag Register, UARTFR" bit definitions
 * ================================================================== */

/* TXFF — 发送 FIFO 满标志 (bit 5)
 * 当此位为 1 时, 发送 FIFO 已满, 不能再写入数据。 */
#define UART_FR_TXFF  (1 << 5)

/* RXFE — 接收 FIFO 空标志 (bit 4)
 * 当此位为 1 时, 接收 FIFO 为空, 没有数据可读。 */
#define UART_FR_RXFE  (1 << 4)

/* ==================================================================
 * UART 基地址
 *
 * QEMU virt 平台的 UART0 映射在物理地址 0x0900_0000。
 * 参考: [QEMU-VIRT] hw/arm/virt.c 中 MemMapEntry 定义
 * ================================================================== */
#define UART0_BASE  0x09000000

/* ==================================================================
 * 静态全局变量: UART 基地址指针
 *
 * volatile: 告诉编译器不要优化对此指针指向内存的读写,
 *           因为 MMIO 寄存器的值可能随时被硬件改变。
 * 参考: C11 标准 §6.7.3 "Type qualifiers"
 * ================================================================== */
static volatile unsigned char *uart_base;

/* ==================================================================
 * uart_init() — 初始化 PL011 UART
 *
 * 初始化步骤:
 *   1. 禁用 UART (写 CR = 0)
 *   2. 清除所有中断 (写 IMSC = 0)
 *   3. 设置波特率 (QEMU 不真正需要, 但保持正确性)
 *   4. 设置 8N1 (8 数据位, 无校验, 1 停止位) + 启用 FIFO
 *   5. 使能 UART、发送、接收
 *
 * 参考: [PL011-TRM] 3.3.9 "Control Register" — 使能顺序
 * ================================================================== */
void uart_init(void)
{
    /* 设置 UART 基地址指针
     * 参考: [QEMU-VIRT] UART0 物理地址 = 0x0900_0000 */
    uart_base = (volatile unsigned char *)UART0_BASE;

    /* 步骤 1: 禁用 UART — 修改设置前必须先禁用
     * CR = 0 → UARTEN(bit0)=0, TXE(bit8)=0, RXE(bit9)=0
     * 参考: [PL011-TRM] 3.3.9 "在修改 UART 配置前应先禁用 UART" */
    *(volatile unsigned int *)(uart_base + UART_CR) = 0;

    /* 步骤 2: 屏蔽所有中断 (本章使用轮询模式, 不需要中断)
     * IMSC = 0 → 所有中断源被屏蔽
     * 参考: [PL011-TRM] 3.3.11 "UARTIMSC" */
    *(volatile unsigned int *)(uart_base + UART_IMSC) = 0;

    /* 步骤 3: 设置波特率
     * 波特率 = UARTCLK / (16 × Divisor)
     * Divisor = IBRD + FBRD/64
     *
     * QEMU virt 的 UART 时钟为 24MHz:
     *   对于 115200 bps: Divisor = 24000000 / (16 × 115200) = 13.0208
     *   IBRD = 13, FBRD = round(0.0208 × 64) = 1
     *
     * 参考: [PL011-TRM] 3.3.6 & 3.3.7 "Baud Rate Divisor" */
    *(volatile unsigned int *)(uart_base + UART_IBRD) = 13;
    *(volatile unsigned int *)(uart_base + UART_FBRD) = 1;

    /* 步骤 4: 配置线控制寄存器
     * LCR_H = 0x70:
     *   bit[6:5] = 0b11 → 8 位字长 (WLEN)
     *   bit[4]   = 1    → 使能 FIFO (FEN)
     *   bit[3]   = 0    → 无奇偶校验
     *   bit[2]   = 0    → 1 个停止位 (不设 STP2)
     *
     * 参考: [PL011-TRM] 3.3.8 "Line Control Register, UARTLCR_H" */
    *(volatile unsigned int *)(uart_base + UART_LCR_H) = (3 << 5) | (1 << 4);

    /* 步骤 5: 使能 UART
     * CR = 0x301:
     *   bit[0] = 1 → UARTEN (UART 使能)
     *   bit[8] = 1 → TXE    (发送使能)
     *   bit[9] = 1 → RXE    (接收使能)
     *
     * 参考: [PL011-TRM] 3.3.9 "Control Register, UARTCR" */
    *(volatile unsigned int *)(uart_base + UART_CR) = (1 << 0) | (1 << 8) | (1 << 9);
}

/* ==================================================================
 * uart_putc() — 发送一个字符
 *
 * 步骤:
 *   1. 轮询等待发送 FIFO 非满 (TXFF == 0)
 *   2. 将字符写入数据寄存器 (DR)
 *
 * 参数:
 *   c — 要发送的字符
 *
 * 参考: [PL011-TRM] 3.3.1 "Data Register" — 写入触发发送
 *        [PL011-TRM] 3.3.3 "Flag Register" — TXFF 标志位
 * ================================================================== */
void uart_putc(char c)
{
    /* 轮询等待: 检查 UARTFR 的 TXFF(bit5) 是否为 0
     * 如果 TXFF=1 表示发送 FIFO 已满, 需要等待硬件取走数据。
     * 参考: [PL011-TRM] 3.3.3 "TXFF, bit[5]: 发送 FIFO 满" */
    while (*(volatile unsigned int *)(uart_base + UART_FR) & UART_FR_TXFF)
        ;   /* 忙等待 (busy wait) */

    /* 将字符写入 UARTDR, 硬件会自动将其放入发送 FIFO 并在串行线上输出。
     * 参考: [PL011-TRM] 3.3.1 "写入 UARTDR 的低 8 位为发送数据" */
    *(volatile unsigned int *)(uart_base + UART_DR) = (unsigned int)c;
}

/* ==================================================================
 * uart_puts() — 发送一个以 '\0' 结尾的字符串
 *
 * 遍历字符串中的每个字符, 逐个调用 uart_putc() 发送。
 * 遇到换行符 '\n' 时, 自动先发送回车 '\r' (串口终端惯例)。
 *
 * 参数:
 *   s — 指向以 null 结尾的字符串的指针
 * ================================================================== */
void uart_puts(const char *s)
{
    /* 遍历字符串直到遇到 null 终止符 '\0' */
    while (*s) {
        /* 串口终端惯例: '\n' (LF) 前需要 '\r' (CR) 才能正确换行。
         * 如果只发送 '\n', 光标只会下移一行但不回到行首。
         * 这被称为 "CR/LF 转换"。 */
        if (*s == '\n')
            uart_putc('\r');       // 先发送回车 (Carriage Return)
        uart_putc(*s);             // 发送当前字符
        s++;                       // 移动到下一个字符
    }
}

/* ==================================================================
 * uart_getc() — 接收一个字符 (阻塞式)
 *
 * 步骤:
 *   1. 轮询等待接收 FIFO 非空 (RXFE == 0)
 *   2. 从数据寄存器 (DR) 读取字符
 *
 * 返回:
 *   接收到的字符 (低 8 位)
 *
 * 参考: [PL011-TRM] 3.3.1 "Data Register" — 读取获得接收数据
 *        [PL011-TRM] 3.3.3 "Flag Register" — RXFE 标志位
 * ================================================================== */
char uart_getc(void)
{
    /* 轮询等待: 检查 UARTFR 的 RXFE(bit4) 是否为 0
     * 如果 RXFE=1 表示接收 FIFO 为空, 没有数据可读。
     * 参考: [PL011-TRM] 3.3.3 "RXFE, bit[4]: 接收 FIFO 空" */
    while (*(volatile unsigned int *)(uart_base + UART_FR) & UART_FR_RXFE)
        ;   /* 忙等待 (busy wait) */

    /* 从 UARTDR 读取低 8 位数据, 即接收到的字符。
     * 参考: [PL011-TRM] 3.3.1 "读取 UARTDR 的低 8 位为接收数据" */
    return (char)(*(volatile unsigned int *)(uart_base + UART_DR) & 0xFF);
}
