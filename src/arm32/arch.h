/* SPDX-License-Identifier: MIT */

/* The 32-bit (ARMv7) side of utils.h: MMIO, CP15 and cache primitives. */

#ifndef ARM32_ARCH_H
#define ARM32_ARCH_H

#include "../types.h"

/*
 * Addresses are u64 throughout m1n1's shared code. Everything these chips
 * have is below 4 GiB, so the accessors take the low half.
 */
#define _MMIO(type, addr) ((volatile type *)(uintptr_t)(addr))

#define _MMIO_ACCESSORS(bits, type)                                                                \
    static inline type read##bits(u64 addr)                                                        \
    {                                                                                              \
        return *_MMIO(type, addr);                                                                 \
    }                                                                                              \
    static inline void write##bits(u64 addr, type data)                                            \
    {                                                                                              \
        *_MMIO(type, addr) = data;                                                                 \
    }                                                                                              \
    static inline type set##bits(u64 addr, type set)                                               \
    {                                                                                              \
        type data = *_MMIO(type, addr) | set;                                                      \
        *_MMIO(type, addr) = data;                                                                 \
        return data;                                                                               \
    }                                                                                              \
    static inline type clear##bits(u64 addr, type clear)                                           \
    {                                                                                              \
        type data = *_MMIO(type, addr) & ~clear;                                                   \
        *_MMIO(type, addr) = data;                                                                 \
        return data;                                                                               \
    }                                                                                              \
    static inline type mask##bits(u64 addr, type clear, type set)                                  \
    {                                                                                              \
        type data = (*_MMIO(type, addr) & ~clear) | set;                                           \
        *_MMIO(type, addr) = data;                                                                 \
        return data;                                                                               \
    }                                                                                              \
    static inline type writeread##bits(u64 addr, type data)                                        \
    {                                                                                              \
        *_MMIO(type, addr) = data;                                                                 \
        return *_MMIO(type, addr);                                                                 \
    }

/* A 64-bit access is two 32-bit ones, low word first; no register here is wider. */
static inline u64 read64(u64 addr)
{
    u32 lo = *_MMIO(u32, addr);
    u32 hi = *_MMIO(u32, addr + 4);
    return lo | ((u64)hi << 32);
}

static inline void write64(u64 addr, u64 data)
{
    *_MMIO(u32, addr) = data;
    *_MMIO(u32, addr + 4) = data >> 32;
}

static inline u64 set64(u64 addr, u64 set)
{
    u64 data = read64(addr) | set;
    write64(addr, data);
    return data;
}

static inline u64 clear64(u64 addr, u64 clear)
{
    u64 data = read64(addr) & ~clear;
    write64(addr, data);
    return data;
}

static inline u64 mask64(u64 addr, u64 clear, u64 set)
{
    u64 data = (read64(addr) & ~clear) | set;
    write64(addr, data);
    return data;
}

static inline u64 writeread64(u64 addr, u64 data)
{
    write64(addr, data);
    return read64(addr);
}

_MMIO_ACCESSORS(32, u32)
_MMIO_ACCESSORS(16, u16)
_MMIO_ACCESSORS(8, u8)

static inline void write64_lo_hi(u64 addr, u64 val)
{
    write32(addr, val);
    write32(addr + 4, val >> 32);
}

/* CP15 registers as "op1, CRn, CRm, op2" */
#define CP15_MIDR    0, c0, c0, 0
#define CP15_CTR     0, c0, c0, 1
#define CP15_MPIDR   0, c0, c0, 5
#define CP15_CCSIDR  1, c0, c0, 0
#define CP15_CLIDR   1, c0, c0, 1
#define CP15_CSSELR  2, c0, c0, 0
#define CP15_SCTLR   0, c1, c0, 0
#define CP15_ACTLR   0, c1, c0, 1
#define CP15_TTBR0   0, c2, c0, 0
#define CP15_TTBCR   0, c2, c0, 2
#define CP15_DACR    0, c3, c0, 0
#define CP15_DFSR    0, c5, c0, 0
#define CP15_IFSR    0, c5, c0, 1
#define CP15_DFAR    0, c6, c0, 0
#define CP15_IFAR    0, c6, c0, 2
#define CP15_ICIALLU 0, c7, c5, 0
#define CP15_ICIMVAU 0, c7, c5, 1
#define CP15_BPIALL  0, c7, c5, 6
#define CP15_DCIMVAC 0, c7, c6, 1
#define CP15_DCISW   0, c7, c6, 2
#define CP15_DCCMVAC 0, c7, c10, 1
#define CP15_DCCSW   0, c7, c10, 2
#define CP15_DCCIMVAC 0, c7, c14, 1
#define CP15_DCCISW  0, c7, c14, 2
#define CP15_TLBIALL 0, c8, c7, 0
#define CP15_VBAR    0, c12, c0, 0
#define CP15_TPIDRPRW 0, c13, c0, 4

#define SCTLR_M  BIT(0)
#define SCTLR_A  BIT(1)
#define SCTLR_C  BIT(2)
#define SCTLR_Z  BIT(11)
#define SCTLR_I  BIT(12)
#define SCTLR_V  BIT(13)
#define SCTLR_TE BIT(30)

#define __cp15_read(op1, crn, crm, op2)                                                            \
    ({                                                                                             \
        u32 __v;                                                                                   \
        __asm__ volatile("mrc\tp15, " #op1 ", %0, " #crn ", " #crm ", " #op2 : "=r"(__v));         \
        __v;                                                                                       \
    })
#define __cp15_write(val, op1, crn, crm, op2)                                                      \
    ({                                                                                             \
        u32 __v = (u32)(val);                                                                      \
        __asm__ volatile("mcr\tp15, " #op1 ", %0, " #crn ", " #crm ", " #op2 : : "r"(__v)          \
                         : "memory");                                                              \
    })
#define _cp15_write(op1, crn, crm, op2, val) __cp15_write(val, op1, crn, crm, op2)
/* cp15_read(CP15_X) and cp15_write(CP15_X, value) */
#define cp15_read(...)  __cp15_read(__VA_ARGS__)
#define cp15_write(...) _cp15_write(__VA_ARGS__)

#define sysop(op) __asm__ volatile(op ::: "memory")

#define ic_ialluis() cp15_write(CP15_ICIALLU, 0)
#define ic_iallu()   cp15_write(CP15_ICIALLU, 0)
#define dc_isw(p)    cp15_write(CP15_DCISW, p)
#define dc_csw(p)    cp15_write(CP15_DCCSW, p)
#define dc_cisw(p)   cp15_write(CP15_DCCISW, p)

#define dma_mb()  sysop("dmb")
#define dma_rmb() sysop("dmb")
#define dma_wmb() sysop("dmb")

static inline u32 read_cpsr(void)
{
    u32 v;
    __asm__ volatile("mrs\t%0, cpsr" : "=r"(v));
    return v;
}

/* None of this exists on a 32-bit chip; the shared code asks anyway. */
static inline bool is_heterogeneous(void)
{
    return false;
}

static inline int is_ecore(void)
{
    return false;
}

static inline int in_el2(void)
{
    return 0;
}

static inline int in_el3(void)
{
    return 0;
}

static inline int has_el3(void)
{
    return 0;
}

static inline int has_el2(void)
{
    return 0;
}

static inline bool is_16k(void)
{
    return false;
}

static inline int is_boot_cpu(void)
{
    return 1;
}

static inline size_t get_page_size(void)
{
    return 4096;
}

/* The chip's free-running timebase (arm32/a32_time.c) */
u64 get_ticks(void);
u64 timer_freq(void);

#endif
