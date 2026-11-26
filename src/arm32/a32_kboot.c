/* SPDX-License-Identifier: MIT */

/*
 * Starting a 32-bit Linux: fill the board's device tree with what only the
 * unit knows (memory, framebuffer, serial number, addresses), put initrd and
 * tree where the kernel's decompressor leaves them alone, and enter the
 * zImage with r0 = 0, r1 = ~0, r2 = device tree, MMU and caches off.
 */

#include "../kboot.h"
#include "../adt.h"
#include "assert.h"
#include "../firmware.h"
#include "../heapblock.h"
#include "malloc.h"
#include "../utils.h"
#include "../xnuboot.h"

#include "../libfdt/libfdt.h"

#define MAX_CHOSEN_PARAMS 16

/*
 * The decompressor finds the start of RAM by rounding its own address down
 * to 128 MiB and unpacks the kernel there, so the zImage has to sit in the
 * first 128 MiB and everything else at or above them.
 */
#define KERNEL_WINDOW (128U << 20)

#define ZIMAGE_MAGIC 0x016f2818

#define DT_ALIGN 4096
#define DT_SLACK (64 * 1024)

static void *dt = NULL;
static int dt_bufsize = 0;
static void *initrd_start = NULL;
static size_t initrd_size = 0;
static char *chosen_params[MAX_CHOSEN_PARAMS][2];

extern const char *const m1n1_version;

#define bail(...)                                                                                  \
    do {                                                                                           \
        printf(__VA_ARGS__);                                                                       \
        return -1;                                                                                 \
    } while (0)

/* reg = <base size> in the cell widths of the tree's root */
static int dt_set_reg(int node, u64 base, u64 size)
{
    fdt32_t cells[4];
    int n = 0;
    int ac = fdt_address_cells(dt, 0);
    int sc = fdt_size_cells(dt, 0);

    if (ac < 1 || ac > 2 || sc < 1 || sc > 2)
        bail("FDT: unsupported root cell sizes %d/%d\n", ac, sc);

    if (ac == 2)
        cells[n++] = cpu_to_fdt32(base >> 32);
    cells[n++] = cpu_to_fdt32(base);
    if (sc == 2)
        cells[n++] = cpu_to_fdt32(size >> 32);
    cells[n++] = cpu_to_fdt32(size);

    return fdt_setprop(dt, node, "reg", cells, n * sizeof(fdt32_t));
}

static int dt_set_cell(int node, const char *name, u64 value)
{
    if (fdt_address_cells(dt, 0) == 2)
        return fdt_setprop_u64(dt, node, name, value);
    return fdt_setprop_u32(dt, node, name, value);
}

static int dt_rename(int node, const char *base, u64 addr)
{
    char name[48];

    snprintf(name, sizeof(name), "%s@%lx", base, addr);
    return fdt_set_name(dt, node, name);
}

/* The simple-framebuffer over the picture iBoot left running, and its reservation */
static int dt_set_fb(void)
{
    if (!cur_boot_args.video.base) {
        printf("FDT: Framebuffer unavailable, skipping framebuffer initialization\n");
        return 0;
    }

    int fb = fdt_node_offset_by_compatible(dt, -1, "simple-framebuffer");
    if (fb < 0) {
        printf("FDT: No framebuffer found\n");
        return 0;
    }

    u64 fb_base = cur_boot_args.video.base;
    u64 fb_size = ALIGN_UP(cur_boot_args.video.stride * cur_boot_args.video.height, SZ_4K);

    /* The reserved range first: renaming nodes moves the ones after them */
    const fdt32_t *region = fdt_getprop(dt, fb, "memory-region", NULL);
    if (region) {
        int rnode = fdt_node_offset_by_phandle(dt, fdt32_to_cpu(*region));

        if (rnode >= 0) {
            if (dt_set_reg(rnode, fb_base, fb_size))
                bail("FDT: couldn't set the framebuffer reservation\n");
            if (dt_rename(rnode, "framebuffer", fb_base))
                bail("FDT: couldn't name the framebuffer reservation\n");
        }
        fb = fdt_node_offset_by_compatible(dt, -1, "simple-framebuffer");
        if (fb < 0)
            bail("FDT: framebuffer node lost\n");
    }

    if (dt_set_reg(fb, fb_base, fb_size))
        bail("FDT: couldn't set framebuffer.reg property\n");
    if (fdt_setprop_u32(dt, fb, "width", cur_boot_args.video.width))
        bail("FDT: couldn't set framebuffer width\n");
    if (fdt_setprop_u32(dt, fb, "height", cur_boot_args.video.height))
        bail("FDT: couldn't set framebuffer height\n");
    if (fdt_setprop_u32(dt, fb, "stride", cur_boot_args.video.stride))
        bail("FDT: couldn't set framebuffer stride\n");

    /* The pixel order stays the board tree's: iBoot's depth does not tell it */
    switch (cur_boot_args.video.depth & 0xff) {
        case 32:
            break;
        case 16:
            if (fdt_setprop_string(dt, fb, "format", "r5g6b5"))
                bail("FDT: couldn't set framebuffer format\n");
            break;
        default:
            printf("FDT: unsupported fb depth %lu, not enabling\n", cur_boot_args.video.depth);
            return 0;
    }

    fdt_delprop(dt, fb, "status");
    if (dt_rename(fb, "framebuffer", fb_base))
        bail("FDT: couldn't set framebuffer name\n");

    printf("FDT: framebuffer base 0x%lx size 0x%lx\n", fb_base, fb_size);
    return 0;
}

static int dt_set_rng_seed(int node)
{
    int anode = adt_path_offset(adt, "/chosen");
    u32 seed_length;

    if (anode < 0)
        return 0;

    const uint8_t *random_seed = adt_getprop(adt, anode, "random-seed", &seed_length);
    if (!random_seed || !seed_length) {
        printf("ADT: no random-seed available!\n");
        return 0;
    }

    if (fdt_setprop(dt, node, "rng-seed", random_seed, seed_length))
        bail("FDT: couldn't set rng-seed\n");
    printf("FDT: Passing %d bytes of random seed\n", seed_length);
    return 0;
}

static int dt_set_chosen(void)
{
    int node = fdt_path_offset(dt, "/chosen");
    if (node < 0)
        bail("FDT: /chosen node not found in devtree\n");

    for (int i = 0; i < MAX_CHOSEN_PARAMS; i++) {
        if (!chosen_params[i][0])
            break;

        const char *name = chosen_params[i][0];
        const char *value = chosen_params[i][1];
        if (!value)
            continue;
        if (fdt_setprop(dt, node, name, value, strlen(value) + 1) < 0)
            bail("FDT: couldn't set chosen.%s property\n", name);
        printf("FDT: %s = '%s'\n", name, value);
    }

    /* A board tree may carry placeholders; with no initrd they must not point anywhere */
    u64 start = (u32)initrd_start;
    u64 end = initrd_size ? start + initrd_size : start;
    if (initrd_size || fdt_getprop(dt, node, "linux,initrd-start", NULL)) {
        if (dt_set_cell(node, "linux,initrd-start", start))
            bail("FDT: couldn't set chosen.linux,initrd-start property\n");
        if (dt_set_cell(node, "linux,initrd-end", end))
            bail("FDT: couldn't set chosen.linux,initrd-end property\n");
    }
    if (initrd_size) {
        if (fdt_add_mem_rsv(dt, start, initrd_size))
            bail("FDT: couldn't add reservation for the initrd\n");
        printf("FDT: initrd at %p size 0x%zx\n", initrd_start, initrd_size);
    }

    if (fdt_setprop(dt, node, "asahi,iboot2-version", os_firmware.iboot,
                    strlen(os_firmware.iboot) + 1))
        bail("FDT: couldn't set asahi,iboot2-version\n");
    if (fdt_setprop(dt, node, "asahi,os-fw-version", os_firmware.string,
                    strlen(os_firmware.string) + 1))
        bail("FDT: couldn't set asahi,os-fw-version\n");
    if (fdt_setprop(dt, node, "asahi,m1n1-stage2-version", m1n1_version, strlen(m1n1_version) + 1))
        bail("FDT: couldn't set asahi,m1n1-stage2-version\n");

    return dt_set_rng_seed(node);
}

static int dt_set_memory(void)
{
    u64 dram_min = cur_boot_args.phys_base;
    u64 dram_size = cur_boot_args.mem_size;

    int node = fdt_path_offset(dt, "/memory");
    if (node < 0)
        bail("FDT: /memory node not found in devtree\n");

    printf("FDT: Usable memory is 0x%lx..0x%lx (0x%lx)\n", dram_min, dram_min + dram_size,
           dram_size);

    if (dt_set_reg(node, dram_min, dram_size))
        bail("FDT: couldn't set memory.reg property\n");
    if (dt_rename(node, "memory", dram_min))
        bail("FDT: couldn't set the memory node's name\n");

    return 0;
}

static int dt_set_serial_number(void)
{
    u32 sn_len;
    const char *serial_number = adt_getprop(adt, 0, "serial-number", &sn_len);

    if (!serial_number || !sn_len || !serial_number[0]) {
        printf("ADT: no serial number\n");
        return 0;
    }
    if (fdt_setprop_string(dt, 0, "serial-number", serial_number))
        bail("FDT: unable to set device serial number!\n");
    printf("FDT: reporting device serial number: %s\n", serial_number);

    return 0;
}

static const struct {
    const char *alias;
    const char *fdt_property;
    bool swap;
} mac_address_devices[] = {
    {.alias = "bluetooth0", .fdt_property = "local-bd-address", .swap = true},
    {.alias = "ethernet0", .fdt_property = "local-mac-address"},
    {.alias = "wifi0", .fdt_property = "local-mac-address"},
};

static int dt_set_mac_addresses(void)
{
    int anode = adt_path_offset(adt, "/chosen");

    if (anode < 0)
        bail("ADT: /chosen not found\n");

    for (size_t i = 0; i < ARRAY_SIZE(mac_address_devices); i++) {
        char propname[32];
        uint8_t addr[6], zero[6] = {0};

        snprintf(propname, sizeof(propname), "mac-address-%s", mac_address_devices[i].alias);
        if (ADT_GETPROP_ARRAY(adt, anode, propname, addr) < 0)
            continue;
        /* iBoot fills these from the unit's configuration; an unfilled one is no address */
        if (!memcmp(addr, zero, sizeof(addr)))
            continue;

        if (mac_address_devices[i].swap) {
            for (size_t j = 0; j < sizeof(addr) / 2; ++j) {
                uint8_t tmp = addr[j];
                addr[j] = addr[sizeof(addr) - j - 1];
                addr[sizeof(addr) - j - 1] = tmp;
            }
        }

        const char *path = fdt_get_alias(dt, mac_address_devices[i].alias);
        if (path == NULL)
            continue;

        int node = fdt_path_offset(dt, path);
        if (node < 0)
            continue;

        fdt_setprop(dt, node, mac_address_devices[i].fdt_property, addr, sizeof(addr));
    }

    return 0;
}

void kboot_set_initrd(void *start, size_t size)
{
    initrd_start = start;
    initrd_size = size;
}

int kboot_set_chosen(const char *name, const char *value)
{
    int i = 0;

    if (!name)
        return -1;

    for (i = 0; i < MAX_CHOSEN_PARAMS; i++) {
        if (!chosen_params[i][0]) {
            chosen_params[i][0] = calloc(strlen(name) + 1, 1);
            strcpy(chosen_params[i][0], name);
            break;
        }

        if (!strcmp(name, chosen_params[i][0])) {
            free(chosen_params[i][1]);
            chosen_params[i][1] = NULL;
            break;
        }
    }

    if (i >= MAX_CHOSEN_PARAMS)
        return -1;

    if (value) {
        chosen_params[i][1] = calloc(strlen(value) + 1, 1);
        strcpy(chosen_params[i][1], value);
    }

    return i;
}

int kboot_set_uboot(const char *name, const char *value)
{
    UNUSED(name);
    UNUSED(value);
    return -1;
}

/* Heap memory at or above the kernel's window, where the decompressor does not write */
static void *alloc_above_kernel(size_t size, size_t align)
{
    u32 floor = (u32)cur_boot_args.phys_base + KERNEL_WINDOW;
    u32 next = (u32)heapblock_alloc_aligned(0, align);

    if (next < floor)
        heapblock_alloc_aligned(floor - next, align);
    return heapblock_alloc_aligned(size, align);
}

int kboot_prepare_dt(void *fdt)
{
    u32 floor = (u32)cur_boot_args.phys_base + KERNEL_WINDOW;

    if (initrd_start && initrd_size && (u32)initrd_start < floor) {
        void *moved = alloc_above_kernel(initrd_size, SZ_4K);

        printf("FDT: moving the initrd from %p to %p\n", initrd_start, moved);
        memcpy(moved, initrd_start, initrd_size);
        initrd_start = moved;
    }

    dt_bufsize = fdt_totalsize(fdt);
    assert(dt_bufsize);

    dt_bufsize += DT_SLACK;
    dt = alloc_above_kernel(dt_bufsize, DT_ALIGN);

    if (fdt_open_into(fdt, dt, dt_bufsize) < 0)
        bail("FDT: fdt_open_into() failed\n");

    if (fdt_add_mem_rsv(dt, (u32)dt, dt_bufsize))
        bail("FDT: couldn't add reservation for the devtree\n");

    if (dt_set_chosen())
        return -1;
    if (dt_set_fb())
        return -1;
    if (dt_set_serial_number())
        return -1;
    if (dt_set_mac_addresses())
        return -1;
    if (dt_set_memory())
        return -1;

    if (fdt_pack(dt))
        bail("FDT: fdt_pack() failed\n");

    printf("FDT prepared at %p\n", dt);

    return 0;
}

int kboot_boot(void *kernel)
{
    u32 base = cur_boot_args.phys_base;

    if (!dt)
        bail("kboot: no device tree prepared\n");
    if ((u32)kernel < base || (u32)kernel >= base + KERNEL_WINDOW)
        bail("kboot: the kernel at %p is outside the first 128 MiB of RAM\n", kernel);

    u32 magic, start, end;
    memcpy(&magic, (u8 *)kernel + 0x24, 4);
    memcpy(&start, (u8 *)kernel + 0x28, 4);
    memcpy(&end, (u8 *)kernel + 0x2c, 4);
    if (magic != ZIMAGE_MAGIC)
        bail("kboot: the kernel at %p is not a zImage\n", kernel);

    /*
     * A kernel built to accept an appended device tree looks for one behind
     * its own end and prefers it to the one in r2. In a payload the board's
     * tree, unfilled, can sit exactly there; it has been copied by now.
     */
    u8 *behind = (u8 *)kernel + (end - start);
    if (fdt_magic(behind) == FDT_MAGIC) {
        printf("kboot: hiding the device tree behind the kernel at %p\n", behind);
        memset(behind, 0, 4);
    }

    printf("Preparing to boot kernel at %p with fdt at %p\n", kernel, dt);

    next_stage.entry = kernel;
    next_stage.args[0] = 0;
    next_stage.args[1] = 0xffffffff; /* no machine number: the device tree says it all */
    next_stage.args[2] = (u32)dt;
    next_stage.args[3] = 0;
    next_stage.args[4] = 0;
    next_stage.restore_logo = false;

    return 0;
}
