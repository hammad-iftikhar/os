#include <stdint.h>
#include "thread.h"
#include "pmm.h"
#include "kprintf.h"

// ponytail: fixed table of 8 threads, no exit/join -- workers run
// forever. Dynamic thread lifecycle when a milestone needs it.
#define NTHREAD 8

extern void swtch(struct context *old, struct context *new);
extern void thread_start(void);

static struct thread threads[NTHREAD];
static int current;

void thread_bootstrap(void)
{
    // kmain is already running on the boot stack; give it a slot so
    // it can be switched away from like everyone else.
    threads[0].state = THREAD_RUNNING;
    threads[0].name = "main";
    current = 0;
}

void thread_create(void (*fn)(void), const char *name)
{
    struct thread *t = 0;

    for (int i = 0; i < NTHREAD; i++) {
        if (threads[i].state == THREAD_UNUSED) {
            t = &threads[i];
            break;
        }
    }
    if (!t)
        panic("thread_create: no free slot for %s", name);

    void *stack = alloc_page();     // 4 KiB kernel stack

    t->name = name;
    t->ctx.x19 = (uint64_t)fn;      // thread_start jumps via x19
    t->ctx.x30 = (uint64_t)thread_start;
    t->ctx.sp = (uint64_t)stack + PAGE_SIZE;    // stacks grow down
    t->state = THREAD_RUNNABLE;

    kprintf("thread: created '%s'\n", name);
}

// Round-robin: next runnable slot after the current one, wrapping.
// Called with IRQs masked (from the timer IRQ path).
void schedule(void)
{
    int next = -1;

    for (int i = 1; i <= NTHREAD; i++) {
        int cand = (current + i) % NTHREAD;
        if (threads[cand].state == THREAD_RUNNABLE) {
            next = cand;
            break;
        }
    }
    if (next < 0)
        return;                     // nobody else wants the CPU

    struct thread *old = &threads[current];
    struct thread *new = &threads[next];

    old->state = THREAD_RUNNABLE;
    new->state = THREAD_RUNNING;
    current = next;
    swtch(&old->ctx, &new->ctx);
    // When someone switches back to us, we resume right here.
}

void thread_return_panic(void);

void thread_return_panic(void)
{
    panic("thread '%s' returned", threads[current].name);
}
