/* SPDX-License-Identifier: MIT */

/*
 * What the shared code expects of the processor on a chip where m1n1 only
 * ever runs on the boot core: Linux starts the second core itself.
 */

#include "../adt.h"
#include "../exception.h"
#include "../smp.h"
#include "../utils.h"

int boot_cpu_idx = 0;
u64 boot_cpu_mpidr = 0;

u8 *secondary_stacks[MAX_CPUS];

void smp_secondary_entry(void)
{
}

void smp_start_secondaries(void)
{
    boot_cpu_mpidr = cp15_read(CP15_MPIDR);
}

void smp_stop_secondaries(bool deep_sleep)
{
    UNUSED(deep_sleep);
}

void smp_call4(int cpu, void *func, u64 arg0, u64 arg1, u64 arg2, u64 arg3)
{
    UNUSED(func);
    UNUSED(arg0);
    UNUSED(arg1);
    UNUSED(arg2);
    UNUSED(arg3);
    printf("SMP: no CPU %d to call here\n", cpu);
}

u64 smp_wait(int cpu)
{
    UNUSED(cpu);
    return 0;
}

bool smp_is_alive(int cpu)
{
    return cpu == boot_cpu_idx;
}

uint64_t smp_get_mpidr(int cpu)
{
    return cpu == boot_cpu_idx ? cp15_read(CP15_MPIDR) : 0;
}

u64 smp_get_release_addr(int cpu)
{
    UNUSED(cpu);
    return 0;
}

void smp_set_wfe_mode(bool new_mode)
{
    UNUSED(new_mode);
}

void smp_send_ipi(int cpu)
{
    UNUSED(cpu);
}

/* One core, interrupts masked: a lock only has to count */
void spin_init(spinlock_t *lock)
{
    lock->lock = -1;
    lock->count = 0;
}

void spin_lock(spinlock_t *lock)
{
    lock->lock = 0;
    lock->count++;
}

void spin_unlock(spinlock_t *lock)
{
    if (lock->count > 0 && !--lock->count)
        lock->lock = -1;
}

void cpu_sleep(bool deep)
{
    UNUSED(deep);
    while (1)
        sysop("wfi");
}

void deep_wfi(void)
{
    sysop("wfi");
}

bool supports_arch_retention(void)
{
    return false;
}

bool supports_gxf(void)
{
    return false;
}

bool supports_pan(void)
{
    return false;
}

/* The 64-bit proxy calls into other exception levels; there are none here */
uint64_t el0_call(void *func, uint64_t a, uint64_t b, uint64_t c, uint64_t d)
{
    UNUSED(func);
    UNUSED(a);
    UNUSED(b);
    UNUSED(c);
    UNUSED(d);
    return -1;
}

uint64_t el1_call(void *func, uint64_t a, uint64_t b, uint64_t c, uint64_t d)
{
    return el0_call(func, a, b, c, d);
}

uint64_t el3_call(void *func, uint64_t a, uint64_t b, uint64_t c, uint64_t d)
{
    return el0_call(func, a, b, c, d);
}

/* Tuning the 64-bit chips want before Linux; nothing to do here */
int cpufreq_init(void)
{
    return 0;
}

void mitigations_perform(void)
{
}

void mitigations_configure(const char *config)
{
    UNUSED(config);
}

int display_configure(const char *config)
{
    UNUSED(config);
    return 0;
}

/*
 * The outer cache (PL310) of the Cortex-A9 chips. iBoot leaves it off and
 * m1n1 never turns it on; should a boot stage have left it on, it is
 * written back and turned off, which is the state Linux expects.
 */
#define L2X0_CTRL          0x100
#define L2X0_CACHE_SYNC    0x730
#define L2X0_CLEAN_INV_WAY 0x7fc

void l2cache_shutdown(void)
{
    int path[8];
    u64 base;

    if (adt_path_offset_trace(adt, "/arm-io/pl310", path) < 0)
        return;
    if (adt_get_reg(adt, path, "reg", 0, &base, NULL))
        return;
    if (!(read32(base + L2X0_CTRL) & 1))
        return;

    write32(base + L2X0_CLEAN_INV_WAY, 0xffff);
    while (read32(base + L2X0_CLEAN_INV_WAY) & 0xffff)
        ;
    write32(base + L2X0_CACHE_SYNC, 0);
    write32(base + L2X0_CTRL, 0);
}

/* Starting another m1n1 from storage is the 64-bit chips' two-stage boot */
int chainload_load(const char *spec, char **chosen, size_t chosen_cnt)
{
    UNUSED(spec);
    UNUSED(chosen);
    UNUSED(chosen_cnt);
    printf("chainload: not available on this chip\n");
    return -1;
}
