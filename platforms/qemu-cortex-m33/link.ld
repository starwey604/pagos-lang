OUTPUT_ARCH(arm)
ENTRY(Reset_Handler)
MEMORY {
    CODE (rx) : ORIGIN = 0x10000000, LENGTH = 4M
    DATA (rw) : ORIGIN = 0x28000000, LENGTH = 2M
}
SECTIONS {
    .isr_vector : { KEEP(*(.isr_vector)) } > CODE
    .text : { *(.text.start) *(.text .text.*) } > CODE
    .rodata : { *(.rodata .rodata.*) } > CODE
    .ARM.extab : { *(.ARM.extab*) } > CODE
    .ARM.exidx : {
        __exidx_start = .;
        *(.ARM.exidx*)
        __exidx_end = .;
    } > CODE
    .data : {
        __data_start = .;
        *(.data .data.*)
        . = ALIGN(4);
        __data_end = .;
    } > DATA AT> CODE
    __data_load = LOADADDR(.data);
    .bss (NOLOAD) : {
        __bss_start = .;
        *(.bss .bss.* COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > DATA
    __stack_top = ORIGIN(DATA) + LENGTH(DATA);
    ASSERT(__bss_end + 65536 < __stack_top, "insufficient stack space")
}
