#include <stdarg.h>
#include <stdint.h>
#include "kprintf.h"
#include "uart.h"

// Print u in the given base, no allocation: fill a buffer backwards.
// 64-bit worst case: 20 decimal digits.
static void print_uint(uint64_t u, unsigned base)
{
    static const char digits[] = "0123456789abcdef";
    char buf[20];
    int i = 0;

    do {
        buf[i++] = digits[u % base];
        u /= base;
    } while (u > 0);

    while (i > 0)
        uart_putc(buf[--i]);
}

void vkprintf(const char *fmt, va_list ap)
{
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            if (*fmt == '\n')
                uart_putc('\r');
            uart_putc(*fmt);
            continue;
        }

        fmt++;                          // skip the '%'
        switch (*fmt) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            uart_puts(s ? s : "(null)");
            break;
        }
        case 'd': {
            int n = va_arg(ap, int);
            unsigned int u = (unsigned int)n;
            if (n < 0) {
                uart_putc('-');
                u = -u;                 // safe even for INT_MIN
            }
            print_uint(u, 10);
            break;
        }
        case 'x':
            print_uint(va_arg(ap, unsigned int), 16);
            break;
        case 'p': {
            // fixed-width: pointers align in register dumps
            uintptr_t p = (uintptr_t)va_arg(ap, void *);
            uart_puts("0x");
            for (int shift = 60; shift >= 0; shift -= 4)
                uart_putc("0123456789abcdef"[(p >> shift) & 0xf]);
            break;
        }
        case 'c':
            uart_putc((char)va_arg(ap, int));   // char promotes to int
            break;
        case '%':
            uart_putc('%');
            break;
        case '\0':
            return;                     // format ends in a lone '%'
        default:                        // unknown specifier: show it
            uart_putc('%');
            uart_putc(*fmt);
            break;
        }
    }
}

void kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vkprintf(fmt, ap);
    va_end(ap);
}

_Noreturn void panic(const char *fmt, ...)
{
    va_list ap;

    kprintf("\nPANIC: ");
    va_start(ap, fmt);
    vkprintf(fmt, ap);
    va_end(ap);
    kprintf("\n");

    for (;;)
        __asm__ volatile("wfe");
}
