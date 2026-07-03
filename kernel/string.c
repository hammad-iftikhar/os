#include "string.h"

// Freestanding code must supply these. The compiler also assumes they
// exist -- clang may lower struct zeroing or copies into calls to
// memset/memcpy even at -ffreestanding.

void *memset(void *dst, int c, unsigned long n)
{
    unsigned char *d = dst;

    while (n--)
        *d++ = (unsigned char)c;
    return dst;
}

void *memcpy(void *dst, const void *src, unsigned long n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;

    while (n--)
        *d++ = *s++;
    return dst;
}
