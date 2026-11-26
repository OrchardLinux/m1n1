/* SPDX-License-Identifier: MIT */

/*
 * Entry from the 32-bit iBoot, which starts m1n1 as its kernel: ARM state,
 * MMU and caches off, r0 = boot arguments (physical). m1n1 is linked at 0
 * and runs wherever iBoot put it.
 */

.syntax unified
.arm

.section .init, "ax"

.globl _start
.type _start, %function
_start:
    cpsid   aif, #0x13          @ SVC, IRQ/FIQ/abort masked
    mov     r8, r0              @ boot arguments

    /* A word holding a symbol reads as its link address until it is relocated */
    adr     r4, _start
    ldr     r5, .L_start
    sub     r9, r4, r5          @ r9 = where the image really starts

    ldr     r0, .L_stack_bot
    add     sp, r0, r9

    mov     r0, r9
    ldr     r1, .L_rel_start
    add     r1, r1, r9
    ldr     r2, .L_rel_end
    add     r2, r2, r9
    bl      apply_rel

    mov     r0, r8
    mov     r1, r9
    bl      _start_c
1:  b       1b

.align 2
.L_start:       .word _start
.L_stack_bot:   .word _stack_bot
.L_rel_start:   .word _rel_start
.L_rel_end:     .word _rel_end

.globl reboot
.type reboot, %function
reboot:
    bl      wdt_reboot
1:  b       1b

/*
 * void next_stage_enter(void *entry, u32 r0, u32 r1, u32 r2)
 * Jump to the next stage with the first three argument registers set.
 */
.globl next_stage_enter
.type next_stage_enter, %function
next_stage_enter:
    mov     r12, r0
    mov     r0, r1
    mov     r1, r2
    mov     r2, r3
    bx      r12
