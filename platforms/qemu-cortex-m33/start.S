.syntax unified
.cpu cortex-m33
.thumb

.section .isr_vector,"a",%progbits
.global vectors
vectors:
    .word __stack_top
    .word Reset_Handler
    .word Fault_Handler
    .word Fault_Handler
    .word Fault_Handler
    .word Fault_Handler
    .word UsageFault_Handler
    .rept 9
    .word Fault_Handler
    .endr

.section .text.start,"ax",%progbits
.thumb_func
.global Reset_Handler
Reset_Handler:
    // Enable UsageFault so a compiler trap differs from an invalid bus access.
    ldr r0, =0xe000ed24
    ldr r1, =0x00040000
    str r1, [r0]
    ldr r0, =__data_load
    ldr r1, =__data_start
    ldr r2, =__data_end
#ifndef SKIP_DATA_COPY
1:  cmp r1, r2
    bhs 2f
    ldr r3, [r0], #4
    str r3, [r1], #4
    b 1b
#endif
2:  ldr r0, =__bss_start
    ldr r1, =__bss_end
    movs r2, #0
    mvns r2, r2
3:  cmp r0, r1
    bhs 4f
    str r2, [r0], #4
    b 3b
4:
#ifndef SKIP_BSS_CLEAR
    ldr r0, =__bss_start
    movs r2, #0
5:  cmp r0, r1
    bhs 6f
    str r2, [r0], #4
    b 5b
#endif
6:
#ifdef WITH_PAGOS_MMIO
#ifdef FORCE_FAILURE
    movs r0, #1
#else
    movs r0, #0
#endif
    bl mmio_main
    bl platform_finish
#else
    bl smoke_main
#endif
    b .

.thumb_func
.global UsageFault_Handler
UsageFault_Handler:
    // Tests run thread mode on MSP; forward its exception frame's saved PC.
    ldr r0, [sp, #24]
    b trap_main
.thumb_func
.global Fault_Handler
Fault_Handler:
    b fault_main

.section .note.GNU-stack,"",%progbits
