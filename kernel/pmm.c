#include <stdint.h>
#include "pmm.h"
#include "string.h"
#include "kprintf.h"

// Free-list allocator. The trick: free pages store the list themselves.
// The first 8 bytes of each free page hold a pointer to the next free
// page -- so tracking N free pages costs zero bytes of metadata.
//
// Managed range: _kernel_end (everything below is kernel image + boot
// stack) up to the end of RAM. RAM size matches the Makefile's -m 128M.

#define RAM_END 0x48000000UL    // 0x40000000 + 128 MiB

struct free_page {
    struct free_page *next;
};

static struct free_page *free_list;
static uint64_t free_count;

void pmm_init(void)
{
    extern char _kernel_end[];
    uint64_t first = (uint64_t)_kernel_end;

    for (uint64_t pa = first; pa + PAGE_SIZE <= RAM_END; pa += PAGE_SIZE)
        free_page((void *)pa);

    kprintf("pmm: %d pages free (%d KiB), first at %p\n",
            (int)free_count, (int)(free_count * PAGE_SIZE / 1024),
            (void *)first);
}

void *alloc_page(void)
{
    struct free_page *p = free_list;

    if (!p)
        panic("pmm: out of memory");
    free_list = p->next;
    free_count--;

    memset(p, 0, PAGE_SIZE);    // callers rely on zeroed pages
    return p;
}

void free_page(void *p)
{
    if ((uint64_t)p % PAGE_SIZE)
        panic("pmm: freeing misaligned pointer %p", p);

    struct free_page *fp = p;

    fp->next = free_list;
    free_list = fp;
    free_count++;
}

uint64_t pmm_free_count(void)
{
    return free_count;
}
