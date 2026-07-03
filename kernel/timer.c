#include <stdint.h>
#include "timer.h"
#include "gic.h"
#include "kprintf.h"

// ARM generic timer, EL1 physical. No MMIO -- the timer lives in
// system registers. CNTFRQ_EL0 = counter frequency (fixed by the
// platform), CNTP_TVAL_EL0 = "fire this many ticks from now",
// CNTP_CTL_EL0 bit 0 = enable.

static uint64_t ticks_per_period;
static uint64_t tick_count;

static inline uint64_t read_cntfrq(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

static inline void write_cntp_tval(uint64_t v)
{
    __asm__ volatile("msr cntp_tval_el0, %0" :: "r"(v));
}

static inline void write_cntp_ctl(uint64_t v)
{
    __asm__ volatile("msr cntp_ctl_el0, %0" :: "r"(v));
}

void timer_init(uint32_t hz)
{
    uint64_t freq = read_cntfrq();

    ticks_per_period = freq / hz;
    kprintf("timer: counter runs at %d Hz, firing every %d ticks\n",
            (int)freq, (int)ticks_per_period);

    gic_enable_intid(TIMER_INTID);
    write_cntp_tval(ticks_per_period);  // first shot
    write_cntp_ctl(1);                  // enable, IRQ unmasked
}

void timer_tick(void)
{
    tick_count++;
    kprintf("tick %d\n", (int)tick_count);
    write_cntp_tval(ticks_per_period);  // re-arm: TVAL counts down anew
}
