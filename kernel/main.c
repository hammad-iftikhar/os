#include "kprintf.h"

void kmain(void)
{
    kprintf("Hello from kernel\n");
    kprintf("string: %s, char: %c, percent: %%\n", "works", 'A');
    kprintf("decimal: %d, negative: %d, int min: %d\n",
            42, -42, -2147483648);
    kprintf("hex: %x, pointer: %p\n", 0xdeadbeef, (void *)kmain);
    kprintf("unknown specifier passes through: %q\n");

    panic("end of milestone 2 demo (deliberate)");
}
