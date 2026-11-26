/* SPDX-License-Identifier: MIT */

/*
 * Time on the 32-bit chips. Their cores have no architected timer; the
 * interrupt controller carries a free-running 64-bit timebase instead
 * (TIME_LO/TIME_HI, 24 MHz), which iBoot and iOS use as well.
 */

#include "../adt.h"
#include "../utils.h"

#define AIC_TIME_LO 0x20
#define AIC_TIME_HI 0x28

#define TIMEBASE_DEFAULT_HZ 24000000

static u64 aic_base;
static u64 timebase_hz = TIMEBASE_DEFAULT_HZ;
static u64 fallback_ticks;

void time_init(void)
{
    int path[8];
    int node = adt_path_offset_trace(adt, "/arm-io/aic", path);

    if (node < 0 || adt_get_reg(adt, path, "reg", 0, &aic_base, NULL)) {
        aic_base = 0;
        printf("time: no interrupt controller timebase, delays are approximate\n");
        return;
    }

    int cpus = adt_path_offset(adt, "/cpus");
    if (cpus >= 0) {
        ADT_FOREACH_CHILD(adt, cpus)
        {
            u32 hz = 0;
            if (ADT_GETPROP(adt, cpus, "timebase-frequency", &hz) >= 0 && hz) {
                timebase_hz = hz;
                break;
            }
        }
    }

    printf("time: timebase at 0x%lx, %lu Hz\n", aic_base + AIC_TIME_LO, timebase_hz);
}

u64 timer_freq(void)
{
    return timebase_hz;
}

u64 get_ticks(void)
{
    if (!aic_base) {
        /* Nothing to read: count calls, so that every wait still ends */
        fallback_ticks += timebase_hz / 100000;
        return fallback_ticks;
    }

    u32 hi, lo, hi2;
    do {
        hi = read32(aic_base + AIC_TIME_HI);
        lo = read32(aic_base + AIC_TIME_LO);
        hi2 = read32(aic_base + AIC_TIME_HI);
    } while (hi != hi2);

    return ((u64)hi << 32) | lo;
}
