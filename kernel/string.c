/*
 * kernel/string.c — 基本字符串/内存操作函数实现
 *
 * 参考文档:
 *   [C11] ISO/IEC 9899:2011 §7.24 "String handling"
 */

#include "string.h"

/* ==================================================================
 * memset() — 将内存区域的每个字节设为指定值
 *
 * 参数:
 *   s — 目标内存起始地址
 *   c — 要填充的值 (转为 unsigned char)
 *   n — 要填充的字节数
 *
 * 返回: s (目标地址)
 *
 * 参考: [C11] §7.24.6.1 "The memset function"
 * ================================================================== */
void *memset(void *s, int c, size_t n)
{
    unsigned char *p = (unsigned char *)s;
    while (n--)
        *p++ = (unsigned char)c;
    return s;
}

/* ==================================================================
 * memcpy() — 从源内存复制 n 字节到目标内存
 *
 * 注意: 源和目标区域不能重叠 (重叠应使用 memmove)。
 *
 * 参考: [C11] §7.24.2.1 "The memcpy function"
 * ================================================================== */
void *memcpy(void *dest, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;
    while (n--)
        *d++ = *s++;
    return dest;
}

/* ==================================================================
 * strlen() — 返回字符串长度 (不包括终止符 '\0')
 *
 * 参考: [C11] §7.24.6.3 "The strlen function"
 * ================================================================== */
size_t strlen(const char *s)
{
    size_t len = 0;
    while (*s++)
        len++;
    return len;
}
