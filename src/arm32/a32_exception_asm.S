/* SPDX-License-Identifier: MIT */

/*
 * ARMv7 exception vectors. m1n1 runs in SVC mode with interrupts masked, so
 * what arrives here is a fault of its own or of something the proxy was
 * asked to poke. Every exception mode shares one stack (they do not nest
 * usefully) and hands C a frame laid out as struct exc_info begins.
 */

.syntax unified
.arm
.text

.align 5
.globl _vectors_start
_vectors_start:
    b       v_reset
    b       v_undef
    b       v_svc
    b       v_pabt
    b       v_dabt
    b       v_reset
    b       v_irq
    b       v_fiq

.macro vector name, type, back
\name:
    sub     lr, lr, #\back      @ the instruction that faulted
    push    {r0-r12, lr}
    mov     r0, #\type
    b       exc_common
.endm

vector v_reset, 0, 0
vector v_undef, 1, 4
vector v_svc,   2, 4
vector v_pabt,  3, 4
vector v_dabt,  4, 8
vector v_irq,   6, 4
vector v_fiq,   7, 4

exc_common:
    mrs     r1, spsr
    push    {r1}
    /* sp and lr of the code that was interrupted (SVC mode) */
    mrs     r2, cpsr
    cps     #0x13
    mov     r3, sp
    mov     r4, lr
    msr     cpsr_c, r2
    push    {r3, r4}

    /* 17 words so far: one more keeps the stack 8-byte aligned for C */
    mov     r1, sp
    sub     sp, sp, #4
    bl      exc_handler
    add     sp, sp, #4

    pop     {r3, r4}
    pop     {r1}
    msr     spsr_cxsf, r1
    ldm     sp!, {r0-r12, pc}^

/* void exc_set_stacks(void *stack_bottom): the one stack of all exception modes */
.globl exc_set_stacks
.type exc_set_stacks, %function
exc_set_stacks:
    mrs     r1, cpsr
    cps     #0x17               @ abort
    mov     sp, r0
    cps     #0x1b               @ undefined
    mov     sp, r0
    cps     #0x12               @ IRQ
    mov     sp, r0
    cps     #0x11               @ FIQ
    mov     sp, r0
    msr     cpsr_c, r1
    bx      lr
