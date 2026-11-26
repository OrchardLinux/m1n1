/* SPDX-License-Identifier: MIT */

#ifndef __EXCEPTION_H__
#define __EXCEPTION_H__

#define SIZEOF_EXC_INFO (64 * 8)

#ifndef __ASSEMBLER__

#include <assert.h>
#include <stdint.h>

#include "types.h"

enum exc_guard_t {
    GUARD_OFF = 0,
    GUARD_SKIP,
    GUARD_MARK,
    GUARD_RETURN,
    GUARD_TYPE_MASK = 0xff,
    GUARD_SILENT = 0x100,
};

#ifdef __arm__
/* The first 17 words are the frame the vectors push (arm32/a32_exception_asm.S) */
struct exc_info {
    u32 sp;
    u32 lr;
    u32 spsr;
    u32 regs[13];
    u32 pc;
    u32 type;
    u32 fsr;
    u32 far;
    u32 cpu_id;
    u32 mpidr;
    void *extra;
};
#else
struct exc_info {
    u64 regs[32];
    u64 spsr;
    u64 elr;
    u64 esr;
    u64 far;
    u64 afsr1;
    u64 sp[3];
    u64 cpu_id;
    u64 mpidr;
    u64 elr_phys;
    u64 far_phys;
    u64 sp_phys;
    void *extra;
};
static_assert(sizeof(struct exc_info) <= SIZEOF_EXC_INFO, "Please increase SIZEOF_EXC_INFO");
static_assert((sizeof(struct exc_info) & 15) == 0, "SIZEOF_EXC_INFO must be a multiple of 16");
#endif

extern volatile enum exc_guard_t exc_guard;
extern volatile int exc_count;

void exception_initialize(void);
void exception_shutdown(void);

#ifndef __arm__
void print_regs(u64 *regs, int el12);
#endif

uint64_t el0_call(void *func, uint64_t a, uint64_t b, uint64_t c, uint64_t d);
uint64_t el1_call(void *func, uint64_t a, uint64_t b, uint64_t c, uint64_t d);
uint64_t el3_call(void *func, uint64_t a, uint64_t b, uint64_t c, uint64_t d);

#endif

#endif
