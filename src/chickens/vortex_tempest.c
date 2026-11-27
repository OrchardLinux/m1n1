/* SPDX-License-Identifier: MIT */

#include "cpu_regs.h"
#include "utils.h"

/*
 * A12 tunables, from the APPLEVORTEX block of XNU's osfmk/arm64/start.s
 * (xnu-6153). The quoted strings are XNU's own comments.
 */

void init_t8020_vortex(int rev)
{
    // Keep DC by-VA ops live for non-coherent DMA, whatever iBoot left here.
    // See features_a12 in chickens.c.
    reg_clr(SYS_IMP_APL_HID4, HID4_DISABLE_DC_MVA | HID4_DISABLE_DC_SW_L2_OPS);

    // "data corruption due to incorrect branch predictor resolution", B0 and later
    if (rev >= 0x10)
        reg_set(SYS_IMP_APL_HID1, HID1_ENABLE_BR_KILL_LIMIT);

    // "segfaults due to IEX clock-gating"
    reg_set(SYS_IMP_APL_HID1, HID1_RCC_FORCE_ALL_IEX_L3_CLKS_ON);

    // "AF2 ordering rules allow ARM device ordering violations"
    reg_set(SYS_IMP_APL_HID4, HID4_FORCE_NS_ORD_LD_REQ_NO_OLDER_LD);

    // "Poisoned cache line crossing younger load is not redirected by older load-barrier"
    reg_set(SYS_IMP_APL_HID3, HID3_DIS_COLOR_OPT);

    // "disable faster launches of uncacheable unaligned stores to workaround load/load
    // ordering violation"
    reg_set(SYS_IMP_APL_HID11, HID11_DIS_X64_NT_LAUNCH_OPT);
}

void init_t8020_tempest(int rev)
{
    UNUSED(rev);

    // As for Vortex.
    reg_clr(SYS_IMP_APL_EHID4, EHID4_DISABLE_DC_MVA | EHID4_DISABLE_DC_SW_L2_OPS);

    // "AF2 ordering rules allow ARM device ordering violations"
    reg_set(SYS_IMP_APL_EHID4, EHID4_FORCE_NS_ORD_LD_REQ_NO_OLDER_LD);

    // "Poisoned younger load is not redirected by older load-acquire"
    reg_set(SYS_IMP_APL_EHID3, EHID3_DIS_COLOR_OPT);

    // "Disable the extension of prefetcher training pipe clock gating, revert to default
    // gating"
    reg_set(SYS_IMP_APL_EHID10, EHID10_RCC_DIS_PWR_SAVE_PRF_CLK_OFF);
}
