ENTRY(_start)

SECTIONS
{
    /* RAM starts at 0x40000000; QEMU places the DTB there. Link above it. */
    . = 0x40080000;

    .text : {
        KEEP(*(.text.boot))     /* _start must come first */
        *(.text*)
    }
    .rodata : { *(.rodata*) }
    .data   : { *(.data*) }
    .bss    : {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        __bss_end = .;
    }

    . = ALIGN(16);              /* AArch64 requires 16-byte-aligned SP */
    . += 0x4000;                /* 16 KiB boot stack */
    __stack_top = .;

    . = ALIGN(4096);
    _kernel_end = .;            /* first byte the allocator may hand out */
}
