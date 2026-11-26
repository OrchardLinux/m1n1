/* SPDX-License-Identifier: MIT */

/*
 * Copies and fills in fixed-size units, as utils.h promises: they read from
 * src and write to dst in <n>-bit units and leave an unaligned tail alone.
 * The 64- and 128-bit ones move 32-bit words, the widest unit here.
 */

.syntax unified
.arm
.text

.macro func name
.globl \name
.type \name, %function
\name:
.endm

func memcpy128
    bic     r2, r2, #15
    b       1f
func memcpy64
    bic     r2, r2, #7
    b       1f
func memcpy32
    bic     r2, r2, #3
1:  cmp     r2, #0
    bxeq    lr
2:  ldr     r3, [r1], #4
    str     r3, [r0], #4
    subs    r2, r2, #4
    bne     2b
    bx      lr

/* void memset64(void *dst, u64 value, size_t size): value in r2:r3, size on the stack */
func memset64
    ldr     r1, [sp]
    bics    r1, r1, #7
    bxeq    lr
1:  str     r2, [r0], #4
    str     r3, [r0], #4
    subs    r1, r1, #8
    bne     1b
    bx      lr

func memset32
    bics    r2, r2, #3
    bxeq    lr
1:  str     r1, [r0], #4
    subs    r2, r2, #4
    bne     1b
    bx      lr

func memcpy16
    bics    r2, r2, #1
    bxeq    lr
1:  ldrh    r3, [r1], #2
    strh    r3, [r0], #2
    subs    r2, r2, #2
    bne     1b
    bx      lr

func memset16
    bics    r2, r2, #1
    bxeq    lr
1:  strh    r1, [r0], #2
    subs    r2, r2, #2
    bne     1b
    bx      lr

func memcpy8
    cmp     r2, #0
    bxeq    lr
1:  ldrb    r3, [r1], #1
    strb    r3, [r0], #1
    subs    r2, r2, #1
    bne     1b
    bx      lr

func memset8
    cmp     r2, #0
    bxeq    lr
1:  strb    r1, [r0], #1
    subs    r2, r2, #1
    bne     1b
    bx      lr
