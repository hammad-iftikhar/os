#include <stdint.h>
#include "vm.h"
#include "pmm.h"
#include "kprintf.h"

// Identity-mapped virtual memory: VA == PA, but with real memory
// attributes at last -- RAM becomes cacheable Normal memory, MMIO
// stays strictly-ordered Device memory.
//
// Layout: 4 KiB granule, 39-bit VA (T0SZ=25) = 3-level walk.
//   L1: 512 entries x 1 GiB   L2: 512 x 2 MiB   L3: 512 x 4 KiB
//
//   L1[0] = 1 GiB Device block  (0x00000000: flash, GIC, UART, ...)
//   L1[1] = table -> L2, 64 x 2 MiB Normal blocks (our 128 MiB RAM)
//
// No L3 needed yet: 2 MiB blocks are fine until user pages need
// per-4KiB permissions (milestone 7).

#define MAIR_DEVICE_nGnRnE  0x00
#define MAIR_NORMAL_WB      0xffUL
// index into MAIR_EL1, set below: attr0 = device, attr1 = normal
#define ATTRIDX_DEVICE      0
#define ATTRIDX_NORMAL      1

#define PTE_VALID       (1UL << 0)
#define PTE_TABLE       (1UL << 1)     // level 0-2: next-level table
#define PTE_BLOCK       (0UL << 1)     // level 1-2: big leaf block
#define PTE_AF          (1UL << 10)    // access flag: MUST be set, or
                                       // first touch = translation fault
#define PTE_SH_INNER    (3UL << 8)     // inner shareable (SMP-safe)
#define PTE_ATTR(i)     ((uint64_t)(i) << 2)

#define PTE_DEVICE      (PTE_VALID | PTE_BLOCK | PTE_ATTR(ATTRIDX_DEVICE) | PTE_AF)
#define PTE_NORMAL      (PTE_VALID | PTE_BLOCK | PTE_ATTR(ATTRIDX_NORMAL) | PTE_AF | PTE_SH_INNER)

#define RAM_BASE        0x40000000UL
#define RAM_SIZE        (128UL << 20)
#define BLOCK_2M        (2UL << 20)

// TCR_EL1: T0SZ=25 (39-bit VA), 4 KiB granule (TG0=0), write-back
// cacheable walks (IRGN0/ORGN0=1), inner shareable (SH0=3),
// EPD1=1 (TTBR1 unused -- walks through it disabled), IPS=40-bit PA.
#define TCR_VALUE ( \
    25UL            /* T0SZ  */ | \
    (1UL << 8)      /* IRGN0 */ | \
    (1UL << 10)     /* ORGN0 */ | \
    (3UL << 12)     /* SH0   */ | \
    (1UL << 23)     /* EPD1  */ | \
    (2UL << 32)     /* IPS   */ )

void vm_init(void)
{
    uint64_t *l1 = alloc_page();    // zeroed: all entries invalid
    uint64_t *l2 = alloc_page();

    // First GiB: devices, identity, strictly ordered.
    l1[0] = 0x00000000UL | PTE_DEVICE;

    // Second GiB: RAM, via an L2 table of 2 MiB Normal blocks.
    l1[1] = (uint64_t)l2 | PTE_VALID | PTE_TABLE;
    for (uint64_t off = 0; off < RAM_SIZE; off += BLOCK_2M)
        l2[off / BLOCK_2M] = (RAM_BASE + off) | PTE_NORMAL;

    uint64_t mair = MAIR_DEVICE_nGnRnE | (MAIR_NORMAL_WB << 8);

    __asm__ volatile(
        "msr mair_el1, %0\n"
        "msr tcr_el1, %1\n"
        "msr ttbr0_el1, %2\n"
        "isb\n"
        :: "r"(mair), "r"(TCR_VALUE), "r"((uint64_t)l1));

    uint64_t sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= (1UL << 0)     // M: MMU on
           | (1UL << 2)     // C: data cache on
           | (1UL << 12);   // I: instruction cache on
    __asm__ volatile(
        "dsb sy\n"          // all table writes complete before...
        "msr sctlr_el1, %0\n"
        "isb\n"             // ...and no instruction runs under old rules
        :: "r"(sctlr));

    kprintf("vm: MMU on, identity map, RAM=Normal MMIO=Device\n");
}
