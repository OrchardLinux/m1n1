/* SPDX-License-Identifier: MIT */

/*
 * The watchdog of the 32-bit chips (wdt-version 1): the whole device is
 * four registers, a 24 MHz counter, two compare values and a control word.
 */

#include "../wdt.h"
#include "../adt.h"
#include "../utils.h"

#define WDT_CUR_TIME  0x0
#define WDT_BITE_TIME 0x4
#define WDT_BARK_TIME 0x8
#define WDT_CTRL      0xc

#define WDT_CTRL_RESET_EN BIT(2)

static u64 wdt_base = 0;

void wdt_disable(void)
{
    int path[8];
    int node = adt_path_offset_trace(adt, "/arm-io/wdt", path);

    if (node < 0) {
        printf("WDT node not found!\n");
        return;
    }

    if (adt_get_reg(adt, path, "reg", 0, &wdt_base, NULL)) {
        printf("Failed to get WDT reg property!\n");
        wdt_base = 0;
        return;
    }

    printf("WDT registers @ 0x%lx\n", wdt_base);
    write32(wdt_base + WDT_CTRL, 0);
    printf("WDT disabled\n");
}

void wdt_reboot(void)
{
    if (!wdt_base)
        return;

    write32(wdt_base + WDT_CTRL, 0);
    write32(wdt_base + WDT_BITE_TIME, 0x100000);
    write32(wdt_base + WDT_CUR_TIME, 0);
    write32(wdt_base + WDT_CTRL, WDT_CTRL_RESET_EN);
}
