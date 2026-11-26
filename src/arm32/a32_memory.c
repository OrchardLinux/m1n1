/* SPDX-License-Identifier: MIT */

/*
 * MMU and caches for the 32-bit chips: one first-level table of 1 MiB
 * sections, everything mapped onto itself. RAM is cached, the device
 * windows the Apple device tree names are device memory, the rest faults.
 * The MMU is only on because ARMv7 has no data cache without it.
 *
 * A mapping is rounded out to whole sections; where two kinds of memory
 * share one, the later call decides.
 */

#include "../memory.h"
#include "../adt.h"
#include "../heapblock.h"
#include "string.h"
#include "../utils.h"
#include "../xnuboot.h"

#define SECTION_SHIFT 20
#define SECTION_SIZE  (1U << SECTION_SHIFT)
#define SECTIONS      4096

#define SEC_TYPE     0x2
#define SEC_B        (1U << 2)
#define SEC_C        (1U << 3)
#define SEC_XN       (1U << 4)
#define SEC_AP_RW    (3U << 10)
#define SEC_TEX(x)   ((u32)(x) << 12)
#define SEC_AP_RO    (1U << 15)

/* No type extension remap (SCTLR.TRE = 0): TEX, C and B say it directly */
#define SEC_NORMAL    (SEC_C | SEC_B)
#define SEC_NORMAL_NC SEC_TEX(1)
#define SEC_DEVICE    SEC_B

#define SCTLR_TRE (1U << 28)
#define SCTLR_AFE (1U << 29)

void dcache_invalidate_all(void);
void mmu_off(void);

uint64_t ram_base = 0;

/*
 * From the heap: the image is loaded a page into RAM, so a static table
 * would not keep the 16 KiB alignment the hardware wants.
 */
static u32 *pagetable;
#define PAGETABLE_SIZE (SECTIONS * sizeof(u32))

static u32 cache_line(void)
{
    /* CTR.DminLine: log2 of the smallest data cache line, in words */
    return 4U << ((cp15_read(CP15_CTR) >> 16) & 0xf);
}

#define CACHE_RANGE_OP(func, ...)                                                                 \
    void func(void *addr, size_t length)                                                           \
    {                                                                                              \
        u32 line = cache_line();                                                                   \
        u32 p = (u32)addr & ~(line - 1);                                                           \
        u32 end = (u32)addr + length;                                                              \
        for (; p < end; p += line)                                                                 \
            cp15_write(__VA_ARGS__, p);                                                                 \
        sysop("dsb");                                                                              \
        sysop("isb");                                                                              \
    }

CACHE_RANGE_OP(ic_ivau_range, CP15_ICIMVAU)
CACHE_RANGE_OP(dc_ivac_range, CP15_DCIMVAC)
CACHE_RANGE_OP(dc_cvac_range, CP15_DCCMVAC)
CACHE_RANGE_OP(dc_cvau_range, CP15_DCCMVAC)
CACHE_RANGE_OP(dc_civac_range, CP15_DCCIMVAC)

void dc_zva_range(void *addr, size_t length)
{
    memset(addr, 0, length);
}

void dcsw_op_all(u64 op_type)
{
    UNUSED(op_type);
}

/*
 * These chips have no cache-coherent DMA. What a device reads or writes
 * behind the processor's back lives in one uncached section.
 */
static u8 *dma_arena;
static size_t dma_used;

void *dma_alloc(size_t align, size_t size)
{
    size_t start = ALIGN_UP(dma_used, align);

    if (!dma_arena || start + size > SECTION_SIZE) {
        printf("MMU: out of uncached memory (%zu bytes wanted)\n", size);
        return NULL;
    }
    dma_used = start + size;
    return dma_arena + start;
}

void dma_release(void)
{
    dma_used = 0;
}

static void tlb_flush(void)
{
    cp15_write(CP15_TLBIALL, 0);
    cp15_write(CP15_BPIALL, 0);
    sysop("dsb");
    sysop("isb");
}

static void map_sections(u64 from, u64 size, u32 attr)
{
    u64 end = ALIGN_UP(from + size, SECTION_SIZE);
    bool live = mmu_active();

    if (end > 0x100000000ULL)
        end = 0x100000000ULL;

    for (u64 p = ALIGN_DOWN(from, SECTION_SIZE); p < end; p += SECTION_SIZE)
        pagetable[p >> SECTION_SHIFT] = (u32)p | attr;

    if (live) {
        /* The table itself is cached memory */
        dc_cvac_range(pagetable, PAGETABLE_SIZE);
        tlb_flush();
    }
}

void mmu_add_mapping(u64 from, u64 to, size_t size, u8 attribute_index, u64 perms)
{
    u32 attr = SEC_TYPE | SEC_AP_RW;

    if (from != to) {
        printf("MMU: only identity mappings here (0x%lx -> 0x%lx)\n", from, to);
        return;
    }

    switch (attribute_index) {
        case MAIR_IDX_NORMAL:
            attr |= SEC_NORMAL;
            break;
        case MAIR_IDX_NORMAL_NC:
            attr |= SEC_NORMAL_NC | SEC_XN;
            break;
        default:
            attr |= SEC_DEVICE | SEC_XN;
            break;
    }
    if (perms & PTE_AP_RO)
        attr |= SEC_AP_RO;

    /* A cached range that turns uncached must not leave lines behind */
    if (mmu_active() && attribute_index != MAIR_IDX_NORMAL)
        dc_civac_range((void *)(u32)ALIGN_DOWN(from, SECTION_SIZE),
                       ALIGN_UP(size + (from & (SECTION_SIZE - 1)), SECTION_SIZE));

    map_sections(from, size, attr);
}

void mmu_rm_mapping(u64 from, size_t size)
{
    map_sections(from, size, 0);
}

void mmu_map_framebuffer(u64 addr, size_t size)
{
    mmu_add_mapping(addr, addr, size, MAIR_IDX_NORMAL_NC, PERM_RW);
}

/* Every "ranges" entry of /arm-io is a window of device registers */
static void mmu_map_mmio(void)
{
    int node = adt_path_offset(adt, "/arm-io");
    u32 len = 0;
    bool mapped = false;

    if (node >= 0) {
        u32 pa_cells = 2, a_cells = 2, s_cells = 2;
        const u32 *ranges = adt_getprop(adt, node, "ranges", &len);

        ADT_GETPROP(adt, 0, "#address-cells", &pa_cells);
        ADT_GETPROP(adt, node, "#address-cells", &a_cells);
        ADT_GETPROP(adt, node, "#size-cells", &s_cells);

        u32 entry = a_cells + pa_cells + s_cells;
        for (u32 i = 0; ranges && pa_cells && s_cells && (i + 1) * entry * 4 <= len; i++) {
            const u32 *r = ranges + i * entry;
            u64 base = r[a_cells] | (pa_cells > 1 ? (u64)r[a_cells + 1] << 32 : 0);
            u64 size = r[a_cells + pa_cells] | (s_cells > 1 ? (u64)r[a_cells + pa_cells + 1] << 32 : 0);

            if (!size || base >> 32)
                continue;
            printf("MMU: Adding Device mapping 0x%lx (0x%lx)\n", base, size);
            mmu_add_mapping(base, base, size, MAIR_IDX_DEVICE_nGnRnE, PERM_RW);
            mapped = true;
        }
    }

    if (!mapped)
        printf("MMU: no /arm-io ranges, no device registers mapped\n");
}

static void mmu_enable(void)
{
    u32 sctlr = cp15_read(CP15_SCTLR);

    dcache_invalidate_all();
    cp15_write(CP15_ICIALLU, 0);
    cp15_write(CP15_TTBCR, 0);
    cp15_write(CP15_TTBR0, (u32)pagetable);
    cp15_write(CP15_DACR, 1); /* domain 0: client */
    tlb_flush();

    sctlr &= ~(SCTLR_A | SCTLR_TRE | SCTLR_AFE | SCTLR_TE);
    sctlr |= SCTLR_M | SCTLR_C | SCTLR_I | SCTLR_Z;
    cp15_write(CP15_SCTLR, sctlr);
    sysop("isb");
}

void mmu_init(void)
{
    u64 ram_size = mem_size_actual ? mem_size_actual : cur_boot_args.mem_size;

    printf("MMU: Initializing...\n");

    /* Whatever the boot stage left on goes off first */
    if (cp15_read(CP15_SCTLR) & (SCTLR_M | SCTLR_C))
        mmu_off();

    ram_base = cur_boot_args.phys_base;
    if (!pagetable)
        pagetable = heapblock_alloc_aligned(PAGETABLE_SIZE, PAGETABLE_SIZE);
    memset(pagetable, 0, PAGETABLE_SIZE);

    printf("MMU: RAM 0x%lx (0x%lx)\n", ram_base, ram_size);
    mmu_add_mapping(ram_base, ram_base, ram_size, MAIR_IDX_NORMAL, PERM_RWX);
    mmu_map_mmio();

    if (!dma_arena)
        dma_arena = heapblock_alloc_aligned(SECTION_SIZE, SECTION_SIZE);
    mmu_add_mapping((u32)dma_arena, (u32)dma_arena, SECTION_SIZE, MAIR_IDX_NORMAL_NC, PERM_RW);

    mmu_enable();

    printf("MMU: SCTLR=0x%x\n", cp15_read(CP15_SCTLR));
    printf("MMU: running with MMU and caches enabled!\n");
}

void mmu_init_secondary(int cpu)
{
    UNUSED(cpu);
}

void mmu_shutdown(void)
{
    printf("MMU: shutting down...\n");
    mmu_off();
    printf("MMU: shutdown successful, clearing caches\n");
}

u64 mmu_disable(void)
{
    u32 sctlr = cp15_read(CP15_SCTLR);

    if (!(sctlr & SCTLR_M))
        return 0;
    mmu_off();
    return 1;
}

void mmu_restore(u64 state)
{
    if (state && !mmu_active())
        mmu_enable();
}
