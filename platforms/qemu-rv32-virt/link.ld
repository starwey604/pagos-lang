OUTPUT_ARCH(riscv)
ENTRY(_start)
MEMORY { RAM (rwx) : ORIGIN = 0x80000000, LENGTH = 16M }
SECTIONS {
    .text : { KEEP(*(.text.start)) *(.text .text.*) } > RAM
    .rodata : { *(.rodata .rodata.*) } > RAM
    .eh_frame : { *(.eh_frame .eh_frame.*) } > RAM
    __data_load = ALIGN(., 16);
    .data 0x80100000 : AT(__data_load) {
        __data_start = .;
        *(.data .data.* .sdata .sdata.*)
        . = ALIGN(4);
        __data_end = .;
    } > RAM
    .bss (NOLOAD) : {
        __bss_start = .;
        *(.bss .bss.* .sbss .sbss.* COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > RAM
    __stack_top = ORIGIN(RAM) + LENGTH(RAM);
    ASSERT(__data_load + SIZEOF(.data) < ADDR(.data), "load image overlaps data")
    ASSERT(__bss_end + 16384 < __stack_top, "insufficient stack space")
}
