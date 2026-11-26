/* SPDX-License-Identifier: MIT */

#include "../adt.h"
#include "../exception.h"
#include "../firmware.h"
#include "../smp.h"
#include "string.h"
#include "../types.h"
#include "../uart.h"
#include "../utils.h"
#include "../xnuboot.h"

/* What the 32-bit iBoot passes its kernel (XNU pexpert/arm/boot.h, revision 1) */
struct boot_args32 {
    u16 revision;
    u16 version;
    u32 virt_base;
    u32 phys_base;
    u32 mem_size;
    u32 top_of_kernel_data;
    struct {
        u32 base;
        u32 display;
        u32 stride;
        u32 width;
        u32 height;
        u32 depth;
    } video;
    u32 machine_type;
    u32 devtree;
    u32 devtree_size;
    char cmdline[CMDLINE_LENGTH_RV1];
    u32 boot_flags;
    u32 mem_size_actual;
};

u64 boot_args_addr;
struct boot_args cur_boot_args;
void *adt;
u64 boot_flags, mem_size_actual;

struct rel_entry {
    u32 off, info;
};

void m1n1_main(void);
void get_device_info(void);
void time_init(void);
void apply_rel(u32 base, struct rel_entry *rel_start, struct rel_entry *rel_end);
void _start_c(void *boot_args, void *base);

extern char _bss_start[0];
extern char _bss_end[0];

#define R_ARM_NONE     0
#define R_ARM_RELATIVE 23

/* Runs before relocation: no globals, no calls */
void apply_rel(u32 base, struct rel_entry *rel_start, struct rel_entry *rel_end)
{
    for (struct rel_entry *e = rel_start; e < rel_end; e++) {
        u32 type = e->info & 0xff;

        if (type == R_ARM_NONE)
            continue; /* left behind by sections the linker dropped */
        if (type != R_ARM_RELATIVE)
            while (1)
                ;
        *(u32 *)(base + e->off) += base;
    }
}

static void dump_boot_args(struct boot_args *ba)
{
    printf("  revision:     %d\n", ba->revision);
    printf("  version:      %d\n", ba->version);
    printf("  virt_base:    0x%lx\n", ba->virt_base);
    printf("  phys_base:    0x%lx\n", ba->phys_base);
    printf("  mem_size:     0x%lx\n", ba->mem_size);
    printf("  top_of_kdata: 0x%lx\n", ba->top_of_kernel_data);
    printf("  video:\n");
    printf("    base:       0x%lx\n", ba->video.base);
    printf("    display:    0x%lx\n", ba->video.display);
    printf("    stride:     0x%lx\n", ba->video.stride);
    printf("    width:      %lu\n", ba->video.width);
    printf("    height:     %lu\n", ba->video.height);
    printf("    depth:      %lubpp\n", ba->video.depth & 0xff);
    printf("    density:    %lu\n", ba->video.depth >> 16);
    printf("  machine_type: %d\n", ba->machine_type);
    printf("  devtree:      %p\n", ba->devtree);
    printf("  devtree_size: 0x%x\n", ba->devtree_size);
    printf("  cmdline:      %s\n", ba->rv1.cmdline);
    printf("  boot_flags:   0x%lx\n", ba->rv1.boot_flags);
    printf("  mem_size_act: 0x%lx\n", ba->rv1.mem_size_actual);

    boot_flags = ba->rv1.boot_flags;
    mem_size_actual = ba->rv1.mem_size_actual;
    if (!mem_size_actual) {
        /* The usable size stops short of what iBoot keeps at the top (framebuffer) */
        mem_size_actual = ALIGN_UP(ba->mem_size, BIT(26));
        printf("Correcting mem_size_actual to 0x%lx\n", mem_size_actual);
    }
}

void _start_c(void *boot_args, void *base)
{
    UNUSED(base);
    struct boot_args32 *ba = boot_args;

    memset(_bss_start, 0, _bss_end - _bss_start);
    boot_args_addr = (u32)boot_args;

    /* The shared code reads the 64-bit shape of the boot arguments */
    cur_boot_args.revision = ba->revision;
    cur_boot_args.version = ba->version;
    cur_boot_args.virt_base = ba->virt_base;
    cur_boot_args.phys_base = ba->phys_base;
    cur_boot_args.mem_size = ba->mem_size;
    cur_boot_args.top_of_kernel_data = ba->top_of_kernel_data;
    cur_boot_args.video.base = ba->video.base;
    cur_boot_args.video.display = ba->video.display;
    cur_boot_args.video.stride = ba->video.stride;
    cur_boot_args.video.width = ba->video.width;
    cur_boot_args.video.height = ba->video.height;
    cur_boot_args.video.depth = ba->video.depth;
    cur_boot_args.machine_type = ba->machine_type;
    cur_boot_args.devtree = (void *)ba->devtree;
    cur_boot_args.devtree_size = ba->devtree_size;
    memcpy(cur_boot_args.rv1.cmdline, ba->cmdline, sizeof(ba->cmdline));
    cur_boot_args.rv1.boot_flags = ba->boot_flags;
    cur_boot_args.rv1.mem_size_actual = ba->mem_size_actual;

    adt = (void *)(ba->devtree - ba->virt_base + ba->phys_base);

    cp15_write(CP15_TPIDRPRW, 0);

    exception_initialize();

    int ret = uart_init();
    UNUSED(ret);

    uart_puts("Initializing");
    get_device_info();
    time_init();

    printf("CPU init (MIDR: 0x%x)...\n", cp15_read(CP15_MIDR));
    printf("boot_args at %p\n", boot_args);

    dump_boot_args(&cur_boot_args);
    printf("\n");

    m1n1_main();
}
