#include <stdint.h>
#include "uart.h"

// PL011 UART on QEMU's virt board. QEMU wires it up pre-initialized,
// so TX is just: wait until FIFO not full, write the data register.
#define UART0_BASE  0x09000000UL
#define UARTDR      (*(volatile uint32_t *)(UART0_BASE + 0x00))
#define UARTFR      (*(volatile uint32_t *)(UART0_BASE + 0x18))
#define UARTFR_TXFF (1u << 5)   // transmit FIFO full

void uart_putc(char c)
{
    while (UARTFR & UARTFR_TXFF)
        ;
    UARTDR = (uint32_t)c;
}

void uart_puts(const char *s)
{
    while (*s) {
        if (*s == '\n')
            uart_putc('\r');    // real terminals want CRLF
        uart_putc(*s++);
    }
}
