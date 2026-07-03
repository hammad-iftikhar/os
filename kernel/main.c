#include "uart.h"

void kmain(void)
{
    uart_puts("Hello from kernel\n");
    for (;;)
        __asm__ volatile("wfe");
}
