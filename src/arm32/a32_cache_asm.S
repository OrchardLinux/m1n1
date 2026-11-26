/* SPDX-License-Identifier: MIT */

/* Whole-cache operations by set and way, and turning the MMU off. */

.syntax unified
.arm
.text

/*
 * Walk every data or unified cache level and apply one set/way operation
 * (CRm of "mcr p15, 0, rX, c7, <CRm>, 2": c6 invalidates, c14 cleans and
 * invalidates). Uses registers only, so it also runs with the cache off
 * and dirty lines still in it. Clobbers r0-r5, r7, r9-r11.
 */
.macro dcache_all crm
    mrc     p15, 1, r0, c0, c0, 1       @ CLIDR
    ands    r3, r0, #0x7000000
    mov     r3, r3, lsr #23             @ levels of cache * 2
    beq     5f
    mov     r10, #0
1:  add     r2, r10, r10, lsr #1
    mov     r1, r0, lsr r2
    and     r1, r1, #7
    cmp     r1, #2
    blt     4f                          @ no data cache at this level
    mcr     p15, 2, r10, c0, c0, 0      @ CSSELR
    isb
    mrc     p15, 1, r1, c0, c0, 0       @ CCSIDR
    and     r2, r1, #7
    add     r2, r2, #4                  @ log2(line length)
    movw    r4, #0x3ff
    ands    r4, r4, r1, lsr #3          @ highest way
    clz     r5, r4
    movw    r7, #0x7fff
    ands    r7, r7, r1, lsr #13         @ highest set
2:  mov     r9, r4
3:  orr     r11, r10, r9, lsl r5
    orr     r11, r11, r7, lsl r2
    mcr     p15, 0, r11, c7, \crm, 2
    subs    r9, r9, #1
    bge     3b
    subs    r7, r7, #1
    bge     2b
4:  add     r10, r10, #2
    cmp     r3, r10
    bgt     1b
5:  mov     r10, #0
    mcr     p15, 2, r10, c0, c0, 0
    dsb
    isb
.endm

/* void dcache_invalidate_all(void): drop everything, for a cache that is off */
.globl dcache_invalidate_all
.type dcache_invalidate_all, %function
dcache_invalidate_all:
    push    {r4-r11, lr}
    dcache_all c6
    pop     {r4-r11, pc}

/*
 * void mmu_off(void): data cache off, written back and emptied, then MMU and
 * instruction cache off. Between turning the cache off and finishing the
 * write-back nothing may read memory, the stack included.
 */
.globl mmu_off
.type mmu_off, %function
mmu_off:
    push    {r4-r11, lr}
    mrc     p15, 0, r0, c1, c0, 0
    bic     r0, r0, #(1 << 2)           @ C
    mcr     p15, 0, r0, c1, c0, 0
    isb
    dcache_all c14
    mrc     p15, 0, r0, c1, c0, 0
    bic     r0, r0, #(1 << 0)           @ M
    bic     r0, r0, #(1 << 12)          @ I
    mcr     p15, 0, r0, c1, c0, 0
    isb
    mov     r0, #0
    mcr     p15, 0, r0, c8, c7, 0       @ TLBIALL
    mcr     p15, 0, r0, c7, c5, 0       @ ICIALLU
    mcr     p15, 0, r0, c7, c5, 6       @ BPIALL
    dsb
    isb
    pop     {r4-r11, pc}
