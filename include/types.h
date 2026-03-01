/*
 * include/types.h — 基本类型定义
 *
 * 由于裸机环境没有标准库, 我们需要自行定义基本类型。
 * 这些类型定义遵循 LP64 数据模型 (ARM64 Linux 使用的模型)。
 *
 * 参考文档:
 *   [ARM-ABI] ARM 64-bit Architecture (AArch64) ABI
 *   [C11] ISO/IEC 9899:2011 §7.20 "Integer types"
 */

#ifndef TYPES_H
#define TYPES_H

/* ==================================================================
 * 固定宽度整数类型
 *
 * ARM64 LP64 模型:
 *   char  = 8 bit,  short = 16 bit,
 *   int   = 32 bit, long  = 64 bit
 *
 * 参考: [ARM-ABI] "5.1 Fundamental Data Types"
 * ================================================================== */
typedef unsigned char       uint8_t;    /* 8 位无符号整数  */
typedef unsigned short      uint16_t;   /* 16 位无符号整数 */
typedef unsigned int        uint32_t;   /* 32 位无符号整数 */
typedef unsigned long       uint64_t;   /* 64 位无符号整数 */

typedef signed char         int8_t;     /* 8 位有符号整数  */
typedef signed short        int16_t;    /* 16 位有符号整数 */
typedef signed int          int32_t;    /* 32 位有符号整数 */
typedef signed long         int64_t;    /* 64 位有符号整数 */

/* ==================================================================
 * 地址和大小类型
 *
 * uintptr_t — 可以无损存储指针值的无符号整数类型
 * size_t    — 内存大小类型 (sizeof 的返回类型)
 *
 * 参考: [C11] §7.20.1.4 "Integer types capable of holding object pointers"
 * ================================================================== */
typedef unsigned long       uintptr_t;  /* 可存储指针值的整数 */
typedef unsigned long       size_t;     /* 内存大小类型 */

/* ==================================================================
 * 布尔类型
 * 参考: [C11] §7.18 "Boolean type and values"
 * ================================================================== */
typedef int                 bool;
#define true                1
#define false               0

/* ==================================================================
 * NULL 指针常量
 * 参考: [C11] §7.19 "Common definitions"
 * ================================================================== */
#define NULL                ((void *)0)

#endif /* TYPES_H */
