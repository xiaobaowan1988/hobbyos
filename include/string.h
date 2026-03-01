/*
 * include/string.h — 基本字符串/内存操作函数
 *
 * 裸机环境没有标准库, 需要自行实现 memset/memcpy 等基本函数。
 *
 * 参考文档:
 *   [C11] ISO/IEC 9899:2011 §7.24 "String handling"
 */

#ifndef STRING_H
#define STRING_H

#include "types.h"

/* memset — 将内存区域填充为指定值
 * 参考: [C11] §7.24.6.1 */
void *memset(void *s, int c, size_t n);

/* memcpy — 复制内存区域 (源和目标不能重叠)
 * 参考: [C11] §7.24.2.1 */
void *memcpy(void *dest, const void *src, size_t n);

/* strlen — 计算字符串长度
 * 参考: [C11] §7.24.6.3 */
size_t strlen(const char *s);

#endif /* STRING_H */
