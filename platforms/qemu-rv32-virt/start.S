    .section .text.start,"ax",@progbits
    .global _start
_start:
    .option push
    .option norelax
    la sp, __stack_top
    .option pop

    # Copy the initialized image from its load address to its run address.
#ifndef SKIP_DATA_COPY
    la t0, __data_load
    la t1, __data_start
    la t2, __data_end
1:
    bgeu t1, t2, 2f
    lw t3, 0(t0)
    sw t3, 0(t1)
    addi t0, t0, 4
    addi t1, t1, 4
    j 1b
#endif
2:
    # Poison first so the test cannot pass just because QEMU zeroed RAM.
    la t0, __bss_start
    la t1, __bss_end
    li t2, -1
3:
    bgeu t0, t1, 4f
    sw t2, 0(t0)
    addi t0, t0, 4
    j 3b
4:
#ifndef SKIP_BSS_CLEAR
    la t0, __bss_start
5:
    bgeu t0, t1, 6f
    sw zero, 0(t0)
    addi t0, t0, 4
    j 5b
#endif
6:
    call smoke_main
7:
    j 7b
