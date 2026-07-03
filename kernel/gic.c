#include <stdint.h>
#include "gic.h"

// GICv2 on QEMU virt (addresses read from QEMU's own device tree):
// distributor at 0x08000000 (global routing), one CPU interface at
// 0x08010000 (per-core delivery, priority masking, ack/EOI).
#define GICD_BASE       0x08000000UL
#define GICC_BASE       0x08010000UL

#define GICD_CTLR       (*(volatile uint32_t *)(GICD_BASE + 0x000))
#define GICD_ISENABLER  ((volatile uint32_t *)(GICD_BASE + 0x100))

#define GICC_CTLR       (*(volatile uint32_t *)(GICC_BASE + 0x000))
#define GICC_PMR        (*(volatile uint32_t *)(GICC_BASE + 0x004))
#define GICC_IAR        (*(volatile uint32_t *)(GICC_BASE + 0x00c))
#define GICC_EOIR       (*(volatile uint32_t *)(GICC_BASE + 0x010))

void gic_init(void)
{
    GICD_CTLR = 1;      // distributor: start forwarding interrupts
    GICC_PMR = 0xff;    // priority mask: let everything through
    GICC_CTLR = 1;      // CPU interface: start signaling this core
}

void gic_enable_intid(uint32_t intid)
{
    // One bit per interrupt, 32 per register. Write-1-to-enable:
    // no read-modify-write needed, so no race.
    GICD_ISENABLER[intid / 32] = 1u << (intid % 32);
}

uint32_t gic_ack(void)
{
    return GICC_IAR & 0x3ff;        // read = "I'm handling this one"
}

void gic_eoi(uint32_t intid)
{
    GICC_EOIR = intid;              // write = "done with this one"
}
