// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple VFL/SwissPPNFTL recovery for the measured 16 GB and 64 GB PPN layouts,
 * and append for the 64 GB layout.
 *
 * Format and replay rules are ported from Orchard's P105 Python reference
 * (ftl_map.py, ftl_load.py, ftl_log.py and ftl_append.py). It was checked
 * against native iOS logical reads. This code contains no captured firmware
 * or device data. Unsupported formats fail before a block device is created.
 */
#include <linux/bitops.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/overflow.h>
#include <linux/rbtree.h>
#include <linux/slab.h>
#include <linux/sort.h>
#include <linux/unaligned.h>
#include <linux/vmalloc.h>

#include "apple-sftl.h"
#include "apple-sftl-vfl.h"
#include "apple-sftl-reclaim.h"
#include "apple-sftl-account.h"

/* Array bounds: the largest supported package. */
#define SFTL_MAX_BLOCKS	2088U
#define SFTL_MAX_SUBBLOCKS 2086U
#define SFTL_MAX_LANES	16U
#define SFTL_MAX_VFL_PAGES 128U
#define SFTL_MAX_PROVIDER (SFTL_MAX_SUBBLOCKS * 8192U)
#define SFTL_UNMAPPED	U32_MAX
#define SFTL_SEQUENCE	GENMASK_ULL(47, 0)
#define SFTL_MAX_CONTEXTS 16U
#define SFTL_META	16U
#define SFTL_CHUNK	4096U
#define SFTL_PAD	0xffff0000U
#define SFTL_TRIM	0xffff0001U
/* cxtSaveSB serializes the role in byte zero of each 8-byte block entry. */
enum sftl_block_role {
	SFTL_CLEAN_FREE = 1,
	SFTL_CLOSED_DATA = 3,
	SFTL_OPEN_DATA = 4,
	SFTL_CONTEXT = 7,
	SFTL_DIRTY_FREE = 9,
};

#define SFTL_FOOTER_CHUNKS 32U
#define SFTL_FAIL_MAX 8U	/* program failures per session before the writer stops */
#define SFTL_RETRY 1		/* the block was given up: offer the page again */
#define SFTL_CLAIM_TIMEOUT (500U * HZ)
#define SFTL_FOOTER_BYTES (SFTL_FOOTER_CHUNKS * SFTL_CHUNK)
#define SFTL_GC_MAP_BYTES (8U * 1024 * 1024) /* per map, on the measured 512 MiB P105 */

/*
 * One measured package and the storage format on it. A lane is one CAU of
 * one bus, lane = cau * 2 + bus. A superblock is the same physical block in
 * every lane that the VFL lists for the FTL; its addresses step through the
 * lanes in order, four logical pages per physical page.
 */
struct sftl_geometry {
	u16 buses, caus, lanes;
	u16 blocks;		/* physical blocks per CAU */
	u16 pages;		/* MLC pages per block */
	u16 subblocks;		/* FTL superblocks, VFL header +12 and +14 */
	u16 vfl_pages;		/* SLC pages in one VFL bank member */
	u16 vfl_first;		/* lowest physical block of a VFL member */
	u16 bootstrap[2];	/* blocks whose page zero can seed the member list */
	u16 nr_bootstrap;
	u16 map_batch;		/* native map records are a multiple of this */
	u32 disk_pages;		/* logical pages exported as the disk */
	u32 lpages;		/* logical pages including the hidden tail */
	bool writer;		/* append, reclamation and checkpoints are modelled */
	bool summary_sequences;	/* a summary may consume one sequence per chunk */
};

/* 64 GB Hynix PPN with 13G36 (iOS 9.3.5) on the media. */
static const struct sftl_geometry sftl_64gb = {
	.buses = 2, .caus = 8, .lanes = 16, .blocks = 2088, .pages = 128,
	.subblocks = 2086, .vfl_pages = 64, .vfl_first = 2056,
	.bootstrap = { 2087 }, .nr_bootstrap = 1,
	.map_batch = APPLE_SFTL_NATIVE_MAP_BATCH,
	.disk_pages = 15625000, .lpages = 15625016, .writer = true,
};

/*
 * 16 GB Hynix PPN with 12H321 (iOS 8.4.1) on the media, as Cascadia
 * measured it: two CAUs per bus, 1064 blocks of 256 pages, banks in the
 * order (bus 0, cau 0), (bus 1, cau 0), (bus 0, cau 1), (bus 1, cau 1),
 * 1055 superblocks of 4096 slots, the VFL in blocks 1062 and 1063 of every
 * bank, 3906250 exported and 16 hidden logical pages. A context has at
 * least 32 range records, rounded up to a multiple of 16, and a summary of
 * 4 or 8 chunks may count its own chunks in its sequence span.
 */
static const struct sftl_geometry sftl_16gb = {
	.buses = 2, .caus = 2, .lanes = 4, .blocks = 1064, .pages = 256,
	.subblocks = 1055, .vfl_pages = 128, .vfl_first = 1062,
	.bootstrap = { 1063, 1062 }, .nr_bootstrap = 2,
	.map_batch = 16,
	.disk_pages = 3906250, .lpages = 3906266, .summary_sequences = true,
};

static const struct sftl_geometry *const sftl_geometries[] = {
	&sftl_64gb,
	&sftl_16gb,
};

struct sftl_run {
	struct rb_node node;
	u32 start, end, provider;
};

/* stamp packs the native 48-bit sequence with its flags and record type. */
struct sftl_record {
	u64 stamp;
	u32 key, address;
};

struct sftl_records {
	struct sftl_record *items;
	u32 count, capacity;
};

struct apple_sftl {
	struct apple_ppn *ppn;
	const struct sftl_geometry *geo;
	struct apple_sftl_vfl_source vfl_source;
	bool vfl_degraded, context_degraded, native_context;
	u8 roles[SFTL_MAX_SUBBLOCKS], block_flags[SFTL_MAX_SUBBLOCKS];
	u16 wear[SFTL_MAX_SUBBLOCKS];
	struct rb_root runs;
	struct sftl_records context;
	struct apple_sftl_user_seq *closed_user_seq;
	u32 nr_runs;
	u16 lanes[SFTL_MAX_BLOCKS];
	DECLARE_BITMAP(reserved, SFTL_MAX_BLOCKS);
	DECLARE_BITMAP(vfl_reserved, SFTL_MAX_BLOCKS);
	DECLARE_BITMAP(unknown_lane_flags, SFTL_MAX_BLOCKS);
	DECLARE_BITMAP(checkpoint_used, SFTL_MAX_BLOCKS);
	DECLARE_BITMAP(old_closed, SFTL_MAX_BLOCKS);
	u64 checkpoint_start;
	u64 checkpoint_sequence, sequence, journal_sequence;
	DECLARE_BITMAP(candidates, SFTL_MAX_BLOCKS);
	DECLARE_BITMAP(fallback_occupied, SFTL_MAX_SUBBLOCKS);
	DECLARE_BITMAP(used, SFTL_MAX_BLOCKS);
	u32 next_candidate, append_block, append_position, append_capacity, spare;
	const struct apple_sftl_checkpoint_ops *checkpoint_ops;
	void *checkpoint_priv;
	u32 footer_runs;
	u64 append_base;
	u8 *footer;
	bool writer_prepared;
	u8 *page;
	u8 oob[APPLE_PPN_OOB_SIZE];
	/* Blocks given up after a failed program in this session. Their live
	 * data moves to the append point; they are never programmed again.
	 */
	DECLARE_BITMAP(abandoned, SFTL_MAX_BLOCKS);
	/* Open blocks whose last programmed page did not read back at load.
	 * torn_after holds the sequence before each such page; its four
	 * sequences may be missing from the journal.
	 */
	DECLARE_BITMAP(torn, SFTL_MAX_BLOCKS);
	u64 torn_after[SFTL_FAIL_MAX];
	u32 nr_torn;
	u32 failures;
	bool evacuate;
	u8 *stage, *move;	/* the page being appended, a page being moved */
	u8 stage_oob[APPLE_PPN_OOB_SIZE], move_oob[APPLE_PPN_OOB_SIZE];
	struct mutex lock; /* serializes reads and the shared page buffer */
	bool failed;
};

static u32 sftl_stride(const struct apple_sftl *sftl)
{
	return sftl->geo->lanes * sftl->geo->pages * 4;
}

static u32 sftl_provider(const struct apple_sftl *sftl)
{
	return sftl->geo->subblocks * sftl_stride(sftl);
}

/* Logical page slots in a block written on the given lanes. */
static u32 sftl_slots(const struct apple_sftl *sftl, u16 lanes)
{
	return hweight16(lanes) * sftl->geo->pages * 4;
}

static u64 sftl_sequence(const u8 *meta)
{
	return get_unaligned_le64(meta) >> 16;
}

static u64 sftl_stamp(const u8 *meta)
{
	return sftl_sequence(meta) | (u64)meta[1] << 48 | (u64)meta[0] << 56;
}

static unsigned int sftl_kind(const struct sftl_record *record)
{
	return record->stamp >> 56;
}

static unsigned int sftl_flags(const struct sftl_record *record)
{
	return (record->stamp >> 48) & 0xff;
}

static bool sftl_erased(const void *data, size_t size)
{
	return !memchr_inv(data, 0xff, size);
}

static int sftl_read_page(struct apple_sftl *sftl, unsigned int bus,
			  unsigned int cau, unsigned int block,
			  unsigned int page, bool context, u8 *data, u8 *oob)
{
	u8 status;
	int ret;

	ret = sftl->ppn->ops->read_page(sftl->ppn->priv, bus, cau, block,
					 page, context, data, oob, &status);
	if (ret)
		return ret;
	/* Refresh requires a writer. A reader must not hide that requirement. */
	if (status != 0x40 && status != 0x49)
		return -EIO;
	if (status == 0x49 && (!sftl_erased(data, APPLE_PPN_PAGE_SIZE) ||
			       !sftl_erased(oob, APPLE_PPN_OOB_SIZE)))
		return -EBADMSG;
	return 0;
}

static int sftl_translate(struct apple_sftl *sftl, u32 address,
			  unsigned int *bus, unsigned int *cau,
			  unsigned int *block, unsigned int *page,
			  unsigned int *chunk)
{
	u32 offset;
	unsigned int lane, wanted, count;
	u16 bitmap;

	if (address >= sftl_provider(sftl))
		return -ERANGE;
	*chunk = address & 3;
	*block = address / sftl_stride(sftl);
	offset = (address % sftl_stride(sftl)) / 4;
	bitmap = sftl->lanes[*block];
	count = hweight16(bitmap);
	if (!count || offset / count >= sftl->geo->pages)
		return -ERANGE;
	*page = offset / count;
	wanted = offset % count;
	for (lane = 0; lane < sftl->geo->lanes; lane++) {
		if (!(bitmap & BIT(lane)))
			continue;
		if (!wanted--)
			break;
	}
	*bus = lane & 1;
	*cau = lane >> 1;
	return 0;
}

static int sftl_read_address(struct apple_sftl *sftl, u32 address,
			     unsigned int *chunk)
{
	unsigned int bus, cau, block, page;
	int ret;

	ret = sftl_translate(sftl, address, &bus, &cau, &block, &page, chunk);
	if (ret)
		return ret;
	return sftl_read_page(sftl, bus, cau, block, page, false,
			      sftl->page, sftl->oob);
}

static struct sftl_run *sftl_find(struct apple_sftl *sftl, u32 logical)
{
	struct rb_node *node = sftl->runs.rb_node;

	while (node) {
		struct sftl_run *run = rb_entry(node, struct sftl_run, node);

		if (logical < run->start)
			node = node->rb_left;
		else if (logical >= run->end)
			node = node->rb_right;
		else
			return run;
	}
	return NULL;
}

static void sftl_insert(struct apple_sftl *sftl, struct sftl_run *run)
{
	struct rb_node **link = &sftl->runs.rb_node, *parent = NULL;

	while (*link) {
		struct sftl_run *entry = rb_entry(*link, struct sftl_run, node);

		parent = *link;
		link = run->start < entry->start ? &parent->rb_left :
						  &parent->rb_right;
	}
	rb_link_node(&run->node, parent, link);
	rb_insert_color(&run->node, &sftl->runs);
	sftl->nr_runs++;
}

static int sftl_add_run(struct apple_sftl *sftl, u32 start, u32 end, u32 provider)
{
	struct sftl_run *run;

	if (start >= end || end > sftl->geo->lpages ||
	    (provider != SFTL_UNMAPPED &&
	     (provider >= sftl_provider(sftl) || end - start > sftl_provider(sftl) - provider)))
		return -EBADMSG;
	run = kmalloc(sizeof(*run), GFP_KERNEL);
	if (!run)
		return -ENOMEM;
	run->start = start;
	run->end = end;
	run->provider = provider;
	sftl_insert(sftl, run);
	return 0;
}

static int sftl_split(struct apple_sftl *sftl, u32 point)
{
	struct sftl_run *left = sftl_find(sftl, point), *right;

	if (!left)
		return -EBADMSG;
	if (left->start == point)
		return 0;
	right = kmalloc(sizeof(*right), GFP_KERNEL);
	if (!right)
		return -ENOMEM;
	right->start = point;
	right->end = left->end;
	right->provider = left->provider == SFTL_UNMAPPED ? SFTL_UNMAPPED :
			  left->provider + point - left->start;
	left->end = point;
	sftl_insert(sftl, right);
	return 0;
}

static bool sftl_contiguous(struct sftl_run *left, struct sftl_run *right)
{
	return left->end == right->start &&
		((left->provider == SFTL_UNMAPPED && right->provider == SFTL_UNMAPPED) ||
		 (left->provider != SFTL_UNMAPPED && right->provider != SFTL_UNMAPPED &&
		  left->provider + left->end - left->start == right->provider));
}

static void sftl_merge(struct apple_sftl *sftl, struct sftl_run *run)
{
	struct rb_node *node = rb_prev(&run->node);
	struct sftl_run *neighbor;

	if (node) {
		neighbor = rb_entry(node, struct sftl_run, node);
		if (sftl_contiguous(neighbor, run)) {
			neighbor->end = run->end;
			rb_erase(&run->node, &sftl->runs);
			kfree(run);
			sftl->nr_runs--;
			run = neighbor;
		}
	}
	node = rb_next(&run->node);
	if (!node)
		return;
	neighbor = rb_entry(node, struct sftl_run, node);
	if (sftl_contiguous(run, neighbor)) {
		run->end = neighbor->end;
		rb_erase(node, &sftl->runs);
		kfree(neighbor);
		sftl->nr_runs--;
	}
}

static int sftl_replace(struct apple_sftl *sftl, u32 start, u32 length, u32 address)
{
	struct sftl_run *run, *new;
	struct rb_node *node, *next;
	u32 end;
	int ret;

	if (!length || start >= sftl->geo->lpages || length > sftl->geo->lpages - start ||
	    (address != SFTL_UNMAPPED &&
	     (address >= sftl_provider(sftl) || length > sftl_provider(sftl) - address)))
		return -EBADMSG;
	end = start + length;
	ret = sftl_split(sftl, start);
	if (ret)
		return ret;
	if (end < sftl->geo->lpages) {
		ret = sftl_split(sftl, end);
		if (ret)
			return ret;
	}
	new = kmalloc(sizeof(*new), GFP_KERNEL);
	if (!new)
		return -ENOMEM;
	run = sftl_find(sftl, start);
	for (node = &run->node; node; node = next) {
		run = rb_entry(node, struct sftl_run, node);
		if (run->start >= end)
			break;
		next = rb_next(node);
		rb_erase(node, &sftl->runs);
		kfree(run);
		sftl->nr_runs--;
	}
	new->start = start;
	new->end = end;
	new->provider = address;
	sftl_insert(sftl, new);
	sftl_merge(sftl, new);
	return 0;
}

static int sftl_record_add(struct sftl_records *records, u64 stamp,
			   u32 key, u32 address)
{
	struct sftl_record *new;
	u32 capacity;

	if (records->count == SFTL_MAX_PROVIDER)
		return -E2BIG;
	if (records->count == records->capacity) {
		capacity = min_t(u32, max(records->capacity * 2, 256U), SFTL_MAX_PROVIDER);
		new = kvrealloc(records->items, array_size(capacity, sizeof(*new)),
				GFP_KERNEL);
		if (!new)
			return -ENOMEM;
		records->items = new;
		records->capacity = capacity;
	}
	records->items[records->count++] = (struct sftl_record) {
		.stamp = stamp, .key = key, .address = address,
	};
	return 0;
}

static int sftl_record_compare(const void *a, const void *b)
{
	u64 left = ((const struct sftl_record *)a)->stamp & SFTL_SEQUENCE;
	u64 right = ((const struct sftl_record *)b)->stamp & SFTL_SEQUENCE;

	return left < right ? -1 : left > right;
}

#define SFTL_VFL_COPIES 4
#define SFTL_VFL_HEADER 0xff

struct sftl_vfl_slot {
	u32 age;
	u8 tag, copies;
};

struct sftl_vfl_scan {
	struct sftl_vfl_slot slots[2][SFTL_MAX_VFL_PAGES];
	u8 sample[APPLE_PPN_PAGE_SIZE + APPLE_PPN_OOB_SIZE];
	u8 reference_data[APPLE_PPN_PAGE_SIZE];
};

static int sftl_vfl_members(struct apple_sftl *sftl)
{
	struct apple_sftl_vfl_member *members = sftl->vfl_source.members;
	unsigned int lane = 0, i, j, seed;
	u16 block = 0;
	u8 status;
	int ret;

	/* An interrupted erase can remove the usual lane-zero seed. Search only
	 * page zero in the measured bootstrap blocks for a surviving member list.
	 * Where both VFL banks are bootstrap blocks, the older one may be erased.
	 */
	for (seed = 0; seed < sftl->geo->nr_bootstrap; seed++) {
		block = sftl->geo->bootstrap[seed];
		for (lane = 0; lane < sftl->geo->lanes; lane++) {
			ret = sftl->ppn->ops->read_page(sftl->ppn->priv, lane & 1, lane >> 1,
							block, 0, true, sftl->page,
							sftl->oob, &status);
			if (ret)
				return ret;
			/* Some nonmembers report 45 at the bootstrap coordinate. Once
			 * discovered, every actual member is reread with strict status.
			 */
			if (status == 0x45)
				continue;
			if (status != 0x40 && status != 0x49)
				return -EIO;
			if (status == 0x40 && get_unaligned_le32(sftl->oob) == 0x00ffff20)
				break;
			if (!sftl_erased(sftl->page, APPLE_PPN_PAGE_SIZE) ||
			    !sftl_erased(sftl->oob, sizeof(sftl->oob)))
				return -EBADMSG;
		}
		if (lane < sftl->geo->lanes)
			break;
	}
	if (seed == sftl->geo->nr_bootstrap)
		return -EBADMSG;
	for (i = 0; i < 8; i++) {
		const u8 *row = sftl->page + 0x100 + 4 * i;

		members[i].block = get_unaligned_le16(row);
		members[i].lane = row[2];
		if (members[i].block < sftl->geo->vfl_first ||
		    members[i].block >= sftl->geo->blocks ||
		    members[i].lane >= sftl->geo->lanes || row[3])
			return -EBADMSG;
		for (j = 0; j < i; j++)
			if (members[j].block == members[i].block && members[j].lane == members[i].lane)
				return -EBADMSG;
		__set_bit(members[i].block, sftl->vfl_reserved);
	}
	if (lane || seed) {
		for (i = 0; i < 8; i++)
			if (members[i].lane == lane && members[i].block == block)
				return 0;
		return -EBADMSG;
	}
	return 0;
}

static int sftl_vfl_read(struct apple_sftl *sftl, unsigned int group, unsigned int copy,
			 unsigned int page)
{
	const struct apple_sftl_vfl_member *member = &sftl->vfl_source.members[group * 4 + copy];

	return sftl_read_page(sftl, member->lane & 1, member->lane >> 1, member->block,
			      page, true, sftl->page, sftl->oob);
}

static int sftl_vfl_read_slot(struct apple_sftl *sftl, struct sftl_vfl_scan *scan,
			      unsigned int group, unsigned int page, struct sftl_vfl_slot *slot)
{
	const struct apple_sftl_vfl_source *source = &sftl->vfl_source;
	unsigned int copy, i;
	int ret;

	for (copy = 0; copy < SFTL_VFL_COPIES; copy++) {
		ret = sftl_vfl_read(sftl, group, copy, page);
		if (ret)
			return ret;
		if (sftl_erased(sftl->oob, sizeof(sftl->oob))) {
			if (!sftl_erased(sftl->page, APPLE_PPN_PAGE_SIZE))
				return -EBADMSG;
			continue;
		}
		if (slot->copies) {
			if (memcmp(scan->sample, sftl->page, APPLE_PPN_PAGE_SIZE) ||
			    memcmp(scan->sample + APPLE_PPN_PAGE_SIZE, sftl->oob,
				   sizeof(sftl->oob)))
				return -EBADMSG;
		} else {
			if (sftl->oob[0] != 0x20 || sftl->oob[3])
				return -EBADMSG;
			slot->tag = sftl->oob[1];
			slot->age = get_unaligned_le32(sftl->oob + 4);
			if (slot->age < BIT(31))
				return -EOPNOTSUPP;
			if (slot->tag == SFTL_VFL_HEADER) {
				if (sftl->oob[2] != 0xff)
					return -EBADMSG;
				for (i = 0; i < 8; i++) {
					const u8 *row = sftl->page + 0x100 + 4 * i;
					const struct apple_sftl_vfl_member *m = &source->members[i];

					if (get_unaligned_le16(row) != m->block ||
					    row[2] != m->lane || row[3])
						return -EBADMSG;
				}
			} else if (!page || slot->tag >= sftl->geo->lanes || sftl->oob[2]) {
				return -EBADMSG;
			}
			memcpy(scan->sample, sftl->page, APPLE_PPN_PAGE_SIZE);
			memcpy(scan->sample + APPLE_PPN_PAGE_SIZE, sftl->oob,
			       sizeof(sftl->oob));
		}
		slot->copies |= BIT(copy);
	}
	return 0;
}

/* Verify the replica set again when a checkpoint or table is consumed.
 * sample then contains the complete first programmed page and its tag.
 */
static int sftl_vfl_reread(struct apple_sftl *sftl, struct sftl_vfl_scan *scan,
			   unsigned int group, unsigned int page)
{
	const struct sftl_vfl_slot *expected = &scan->slots[group][page];
	struct sftl_vfl_slot actual = {};
	int ret = sftl_vfl_read_slot(sftl, scan, group, page, &actual);

	if (ret)
		return ret;
	if (!actual.copies || actual.copies != expected->copies ||
	    actual.age != expected->age || actual.tag != expected->tag)
		return -EBADMSG;
	return 0;
}

static int sftl_vfl_index(struct apple_sftl *sftl, struct sftl_vfl_scan *scan)
{
	struct apple_sftl_vfl_source *source = &sftl->vfl_source;
	unsigned int group, page;
	bool have_checkpoint = false;
	int ret;

	for (group = 0; group < 2; group++) {
		for (page = 0; page < sftl->geo->vfl_pages; page++) {
			struct sftl_vfl_slot *slot = &scan->slots[group][page];

			ret = sftl_vfl_read_slot(sftl, scan, group, page, slot);
			if (ret)
				return ret;
			if (!slot->copies)
				continue;
			if (!scan->slots[group][0].copies ||
			    (u64)slot->age + page != scan->slots[group][0].age)
				return -EBADMSG;
			if (group && slot->age <= scan->slots[0][0].age) {
				u32 other = scan->slots[0][0].age - slot->age;

				if (other < sftl->geo->vfl_pages && scan->slots[0][other].copies)
					return -EBADMSG;
			}
			if (slot->copies != 0xf) {
				sftl->vfl_degraded = true;
				continue;
			}
			if (slot->tag == SFTL_VFL_HEADER &&
			    (!have_checkpoint || slot->age < source->age)) {
				have_checkpoint = true;
				source->age = slot->age;
				source->group = group;
				source->page = page;
			}
		}
		if (scan->slots[group][0].copies != 0xf)
			sftl->vfl_degraded = true;
	}
	return have_checkpoint ? 0 : -EBADMSG;
}

static int sftl_vfl_load_refs(struct apple_sftl *sftl, struct sftl_vfl_scan *scan,
			      unsigned int group, unsigned int page, u16 *refs)
{
	const struct sftl_vfl_slot *header = &scan->slots[group][page];
	unsigned int lane;
	int ret;

	ret = sftl_vfl_reread(sftl, scan, group, page);
	if (ret)
		return ret;
	/* The 13G36 loader compares FTLSuBlks (+12) with VFLSuBlks
	 * (+14). An older, smaller FTL is a different address space.
	 */
	if (get_unaligned_le16(scan->sample + 10) != 16 ||
	    get_unaligned_le16(scan->sample + 12) != sftl->geo->subblocks ||
	    get_unaligned_le16(scan->sample + 14) != sftl->geo->subblocks)
		return -EOPNOTSUPP;
	for (lane = 0; lane < sftl->geo->lanes; lane++) {
		const struct sftl_vfl_slot *table;
		u16 ref = get_unaligned_le16(scan->sample + 0x200 + 32 * lane);
		u16 row = ref & 0x7fff;

		if (row >= sftl->geo->vfl_pages)
			return -EBADMSG;
		table = &scan->slots[ref >> 15][row];
		/* A stale reference can point at the same lane in a later bank cycle. */
		if (table->copies != 0xf || table->tag != lane || table->age <= header->age)
			return -EBADMSG;
		refs[lane] = ref;
	}
	return 0;
}

static int sftl_vfl_same_table(struct apple_sftl *sftl, struct sftl_vfl_scan *scan,
			       u16 ref, unsigned int group, unsigned int page)
{
	unsigned int block, subblocks = sftl->geo->subblocks, blocks = sftl->geo->blocks;
	int ret;

	ret = sftl_vfl_reread(sftl, scan, ref >> 15, ref & 0x7fff);
	if (ret)
		return ret;
	memcpy(scan->reference_data, scan->sample, APPLE_PPN_PAGE_SIZE);
	ret = sftl_vfl_reread(sftl, scan, group, page);
	if (ret)
		return ret;
	if (memcmp(scan->reference_data, scan->sample, 2 * subblocks) ||
	    memcmp(scan->reference_data + 2 * blocks, scan->sample + 2 * blocks,
		   APPLE_PPN_PAGE_SIZE - 2 * blocks))
		return -EBADMSG;
	/* Two trailing physical indices are outside every supported FTL
	 * address. Older inactive marker flags can differ during rotation.
	 */
	for (block = subblocks; block < blocks; block++) {
		u16 old = get_unaligned_le16(scan->reference_data + 2 * block);
		u16 new = get_unaligned_le16(scan->sample + 2 * block);

		if (old != new && !(old & new & BIT(15)))
			return -EBADMSG;
	}
	return 0;
}

/* During healthy rotation, unpublished table copies reproduce the source
 * payload. An updated table that the selected checkpoint does not name can
 * change ownership: a previous map alone is then insufficient for recovery.
 */
static int sftl_vfl_check_tail(struct apple_sftl *sftl, struct sftl_vfl_scan *scan,
			       const u16 *refs)
{
	const struct apple_sftl_vfl_source *source = &sftl->vfl_source;
	u16 other[SFTL_MAX_LANES];
	unsigned int group, page, lane;
	int ret;

	for (group = 0; group < 2; group++)
		for (page = 0; page < sftl->geo->vfl_pages; page++) {
			const struct sftl_vfl_slot *slot = &scan->slots[group][page];

			if (!slot->copies || slot->age > source->age ||
			    (group == source->group && page == source->page))
				continue;
			if (slot->age == source->age)
				return -EBADMSG;
			sftl->vfl_degraded = true;
			if (slot->tag != SFTL_VFL_HEADER) {
				ret = sftl_vfl_same_table(sftl, scan, refs[slot->tag], group, page);
				if (ret)
					return ret;
				continue;
			}
			ret = sftl_vfl_load_refs(sftl, scan, group, page, other);
			if (ret)
				return ret;
			for (lane = 0; lane < sftl->geo->lanes; lane++) {
				if (other[lane] == refs[lane])
					continue;
				ret = sftl_vfl_same_table(sftl, scan, refs[lane],
							  other[lane] >> 15, other[lane] & 0x7fff);
				if (ret)
					return ret;
			}
		}
	return 0;
}

static int sftl_vfl(struct apple_sftl *sftl)
{
	const struct apple_sftl_vfl_source *source = &sftl->vfl_source;
	struct sftl_vfl_scan *scan;
	u16 refs[SFTL_MAX_LANES];
	unsigned int lane, block;
	int ret;

	ret = sftl_vfl_members(sftl);
	if (ret)
		return ret;
	scan = kvzalloc(sizeof(*scan), GFP_KERNEL);
	if (!scan)
		return -ENOMEM;
	ret = sftl_vfl_index(sftl, scan);
	if (ret)
		goto out;
	ret = sftl_vfl_load_refs(sftl, scan, source->group, source->page, refs);
	if (ret)
		goto out;
	ret = sftl_vfl_check_tail(sftl, scan, refs);
	if (ret)
		goto out;
	for (lane = 0; lane < sftl->geo->lanes; lane++) {
		u16 ref = refs[lane];

		if (!source->page && (ref >> 15) != source->group)
			sftl->vfl_degraded = true;
		ret = sftl_vfl_reread(sftl, scan, ref >> 15, ref & 0x7fff);
		if (ret)
			goto out;
		for (block = 0; block < sftl->geo->blocks; block++) {
			u16 value = get_unaligned_le16(scan->sample + block * 2);

			if (!(value & BIT(15))) {
				sftl->lanes[block] |= BIT(lane);
				if (value)
					__set_bit(block, sftl->unknown_lane_flags);
			}
		}
	}
out:
	kvfree(scan);
	return ret;
}

int apple_sftl_vfl_source(struct apple_sftl *sftl, struct apple_sftl_vfl_source *out)
{
	int ret = 0;

	if (!sftl || !out)
		return -EINVAL;
	mutex_lock(&sftl->lock);
	if (sftl->vfl_degraded || sftl->context_degraded || sftl->failed)
		ret = -EROFS;
	else
		*out = sftl->vfl_source;
	mutex_unlock(&sftl->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(apple_sftl_vfl_source);

/* Qualified SLC operations also exclude writes through raw MTD. The mount's
 * mutex keeps foreground writes, checkpoints and bank rotation serialized.
 */
int apple_sftl_rotate_vfl(struct apple_sftl *sftl, const struct apple_sftl_vfl_ops *ops,
			 void *priv)
{
	struct apple_sftl *cold;
	bool started = false;
	int ret;

	if (!sftl || !ops || !ops->erase || !ops->program || !sftl->geo->writer)
		return -EOPNOTSUPP;
	cold = kvzalloc(sizeof(*cold), GFP_NOIO);
	if (!cold)
		return -ENOMEM;
	mutex_lock(&sftl->lock);
	if (sftl->failed) {
		ret = -EIO;
		goto out;
	}
	if (sftl->vfl_degraded || sftl->context_degraded) {
		ret = -EROFS;
		goto out;
	}
	ret = apple_sftl_vfl_rotate_checked(sftl->ppn, &sftl->vfl_source, ops, priv, &started);
	if (ret)
		goto out;
	cold->ppn = sftl->ppn;
	cold->geo = sftl->geo;
	cold->page = sftl->page;
	ret = sftl_vfl(cold);
	if (ret)
		goto out;
	if (cold->vfl_degraded || cold->vfl_source.group == sftl->vfl_source.group ||
	    cold->vfl_source.page != sftl->geo->lanes + 1 ||
	    cold->vfl_source.age >= sftl->vfl_source.age ||
	    memcmp(cold->vfl_source.members, sftl->vfl_source.members, sizeof(sftl->vfl_source.members)) ||
	    memcmp(cold->lanes, sftl->lanes, sizeof(sftl->lanes)) ||
	    memcmp(cold->vfl_reserved, sftl->vfl_reserved, sizeof(sftl->vfl_reserved)) ||
	    memcmp(cold->unknown_lane_flags, sftl->unknown_lane_flags, sizeof(sftl->unknown_lane_flags))) {
		ret = -EBADMSG;
		goto out;
	}
	sftl->vfl_source = cold->vfl_source;
out:
	if (ret && started)
		sftl->failed = true;
	mutex_unlock(&sftl->lock);
	kvfree(cold);
	return ret;
}
EXPORT_SYMBOL_GPL(apple_sftl_rotate_vfl);


static int sftl_context_names(struct apple_sftl *sftl, const u8 *payload,
			      u16 *pending, u32 *count)
{
	u32 names = get_unaligned_le32(payload), i;

	if (!names || names > SFTL_MAX_CONTEXTS)
		return -EBADMSG;
	for (i = 0; i < names; i++) {
		u32 name = get_unaligned_le32(payload + 4 + i * 4);

		if (!name || name >= sftl->geo->subblocks)
			return -EBADMSG;
		if (test_bit(name, sftl->reserved))
			continue;
		if (*count == SFTL_MAX_CONTEXTS)
			return -E2BIG;
		__set_bit(name, sftl->reserved);
		pending[(*count)++] = name;
	}
	return 0;
}

static void sftl_clear_runs(struct apple_sftl *sftl)
{
	struct rb_node *node;

	while ((node = rb_first(&sftl->runs))) {
		rb_erase(node, &sftl->runs);
		kfree(rb_entry(node, struct sftl_run, node));
	}
	sftl->nr_runs = 0;
}

static int sftl_load_ranges(struct apple_sftl *sftl, const u8 *payload, u32 *cursor,
			    bool *short_range)
{
	u32 value = get_unaligned_le32(payload + 4), offset, lpages = sftl->geo->lpages;

	if (value == U32_MAX)
		return *cursor == lpages && get_unaligned_le32(payload) == U32_MAX ?
			0 : -EBADMSG;
	if (value != 0xfffffff0 || get_unaligned_le32(payload) != *cursor ||
	    *cursor == lpages)
		return -EBADMSG;

	for (offset = 8; offset < SFTL_CHUNK; offset += 8) {
		u32 length = get_unaligned_le32(payload + offset + 4);
		int ret;

		value = get_unaligned_le32(payload + offset);
		if (value == U32_MAX)
			break;
		if (!length || length > lpages - *cursor)
			return -EBADMSG;
		if (value == BIT(fls(sftl_provider(sftl) * 2 - 1) - 1) - 1)
			value = SFTL_UNMAPPED;
		ret = sftl_add_run(sftl, *cursor, *cursor + length, value);
		if (ret)
			return ret;
		*cursor += length;
	}
	/* A save interrupted between programs can end only after complete
	 * 511-pair fragments until the whole logical address space is covered.
	 */
	if (offset != SFTL_CHUNK && *cursor < lpages)
		*short_range = true;
	return offset == 8 ? -EBADMSG : 0;
}

/* Native initial allocation takes an erased free block. A blank head alone
 * can also describe an unnamed context spare, a reserved block or a stream.
 * An older saved role can veto a new allocation while that fallback exists.
 */
static int sftl_load_block_roles(struct apple_sftl *sftl, const u8 *meta, const u8 *payload,
				 bool selected)
{
	u32 block = get_unaligned_le16(meta + 8), count = get_unaligned_le16(meta + 10);

	while (count--) {
		u8 role = payload[0];
		u16 live = get_unaligned_le16(payload + 2);

		if (selected) {
			if (role == SFTL_CONTEXT && live)
				return -EBADMSG;
			if ((role == SFTL_CLEAN_FREE || role == SFTL_DIRTY_FREE) && live)
				role = 0;
			sftl->roles[block] = role;
			sftl->block_flags[block] = payload[1];
			sftl->wear[block] = get_unaligned_le16(payload + 6);
		} else if (role != SFTL_CLEAN_FREE || live) {
			__set_bit(block, sftl->fallback_occupied);
		}
		block++;
		payload += 8;
	}
	return 0;
}

/* -ENODATA distinguishes a valid prefix from malformed records or a read error. */
static int sftl_decode_generation(struct apple_sftl *sftl, const struct sftl_records *records,
				  u32 first, u32 end, unsigned long *named)
{
	u32 cursor = 0, map_records = 0, i, previous_order = 0, stride = sftl_stride(sftl);
	u32 subblocks = sftl->geo->subblocks;
	u64 first_sequence = records->items[first].stamp & SFTL_SEQUENCE;
	bool base_tags = true, at_base = false, native = false, short_range = false;

	for (i = first; i < end; i++) {
		const struct sftl_record *record = &records->items[i];
		u32 index = i - first, subtype = sftl_flags(record), chunk;
		u32 order = subtype == 4 ? 7 : subtype;
		const u8 *meta, *payload;
		int ret;

		if (!first_sequence || (record->stamp & SFTL_SEQUENCE) != first_sequence + index ||
		    order < previous_order || (record->address & 3) != (index & 3))
			return -EBADMSG;
		previous_order = order;
		ret = sftl_read_address(sftl, record->address, &chunk);
		if (ret)
			return ret;
		meta = sftl->oob + chunk * SFTL_META;
		payload = sftl->page + chunk * SFTL_CHUNK;
		if (sftl_stamp(meta) != record->stamp)
			return -EBADMSG;
		if (!index) {
			u32 names = get_unaligned_le32(payload), k;

			if (subtype != 1 || !names || names > SFTL_MAX_CONTEXTS)
				return -EBADMSG;
			at_base = !(record->address % stride) &&
				  get_unaligned_le32(payload + 4) == record->address / stride;
			for (k = 0; k < names; k++) {
				u32 block = get_unaligned_le32(payload + 4 + k * 4);

				if (!block || block >= subblocks ||
				    !test_bit(block, sftl->reserved) || test_bit(block, named))
					return -EBADMSG;
				__set_bit(block, named);
			}
		}
		if (!test_bit(record->address / stride, named))
			return -EBADMSG;
		if (index < 4 && subtype == 1) {
			base_tags &= apple_sftl_native_context_tag_for(subblocks, index, meta);
			if (index == 3) {
				if (!at_base || !base_tags ||
				    get_unaligned_le32(payload + SFTL_CHUNK - 4) != subblocks)
					return -EBADMSG;
				native = true;
			}
		}
		if (native && !apple_sftl_native_context_tag_for(subblocks, index, meta))
			return -EBADMSG;
		if (native && subtype == 3) {
			ret = sftl_load_block_roles(sftl, meta, payload, true);
			if (ret)
				return ret;
		}
		if (subtype == 4) {
			ret = sftl_load_ranges(sftl, payload, &cursor, &short_range);
			if (ret)
				return ret;
			map_records++;
		}
	}

	/* cxtNextBatch pads the measured native map in groups of 64. Coverage
	 * alone can become complete many pages before the final batch does.
	 */
	if (cursor != sftl->geo->lpages || !map_records ||
	    (native && map_records % sftl->geo->map_batch)) {
		if (short_range)
			return -EBADMSG;
		/* A partial program or a native aggregate that lost its tail does
		 * not establish the next safe write sequence. Recover for reads.
		 */
		if (native || (records->items[end - 1].address & 3) != 3)
			sftl->context_degraded = true;
		return -ENODATA;
	}
	sftl->native_context = native;
	return 0;
}

/* Losing the first lane while retiring an old context also loses its base
 * header. The selected checkpoint can still identify the physical block as
 * reserved. Accept only older native records at their original page offsets,
 * with whole-lane erasure or an erased tail on each surviving lane.
 */
static int sftl_check_context_remnant(struct apple_sftl *sftl, u32 block, u64 selected)
{
	u32 streams = hweight16(sftl->lanes[block]), pos, j;
	u16 ended = 0;
	u64 base = 0;

	if (!streams)
		return -EBADMSG;
	for (pos = 0; pos < streams * sftl->geo->pages; pos++) {
		u16 lane = BIT(pos % streams);
		unsigned int chunk;
		int ret = sftl_read_address(sftl, block * sftl_stride(sftl) + pos * 4, &chunk);

		if (ret) {
			if (ret == -EBADMSG)
				return ret;
			/* This site's unread contents cannot establish a writable
			 * medium. The complete checkpoint still serves reads.
			 */
			sftl->context_degraded = true;
			return 0;
		}
		if (sftl_erased(sftl->oob, sizeof(sftl->oob))) {
			if (!sftl_erased(sftl->page, APPLE_PPN_PAGE_SIZE))
				return -EBADMSG;
			ended |= lane;
			continue;
		}
		if (ended & lane)
			return -EBADMSG;
		if (!pos && (!get_unaligned_le32(sftl->page) ||
		    get_unaligned_le32(sftl->page) > SFTL_MAX_CONTEXTS ||
		    get_unaligned_le32(sftl->page + 4) != block ||
		    get_unaligned_le32(sftl->page + APPLE_PPN_PAGE_SIZE - 4) != sftl->geo->subblocks))
			return -EBADMSG;
		for (j = 0; j < 4; j++) {
			const u8 *meta = sftl->oob + SFTL_META * j;
			u32 offset = 4 * pos + j;
			u64 sequence = sftl_sequence(meta);

			if (!apple_sftl_native_context_tag_for(sftl->geo->subblocks, offset, meta) ||
			    sequence <= offset ||
			    sequence >= selected || (base && sequence - offset != base))
				return -EBADMSG;
			base = sequence - offset;
		}
	}
	if (base)
		sftl->context_degraded = true;
	return 0;
}

static void sftl_mark_mapped(struct apple_sftl *sftl, unsigned long *blocks);

static int sftl_context(struct apple_sftl *sftl, const u8 *heads, struct sftl_records *journal)
{
	struct sftl_records records = {};
	DECLARE_BITMAP(gapped, SFTL_MAX_SUBBLOCKS) = {};
	DECLARE_BITMAP(named, SFTL_MAX_SUBBLOCKS);
	u16 pending[SFTL_MAX_CONTEXTS];
	u32 count = 0, block, at, i, first = 0, end, chunk, oldest;
	u32 stride = sftl_stride(sftl), subblocks = sftl->geo->subblocks;
	int ret = -EBADMSG;

	for (block = 1; block < subblocks; block++) {
		if (heads[block * SFTL_META] != 0x1f)
			continue;
		if (count == SFTL_MAX_CONTEXTS)
			goto out;
		pending[count++] = block;
		__set_bit(block, sftl->reserved);
	}
	for (at = 0; at < count; at++) {
		u16 blank_lanes = 0, populated = 0;
		u32 position, pages, lanes;
		bool gap = false;

		block = pending[at];
		lanes = hweight16(sftl->lanes[block]);
		pages = lanes * sftl->geo->pages;
		if (!pages) {
			ret = -EBADMSG;
			goto out;
		}
		for (position = 0; position < pages; position++) {
			u16 lane = BIT(position % lanes);
			bool empty = false;

			ret = sftl_read_address(sftl, block * stride + position * 4, &chunk);
			if (ret)
				goto out;
			if (sftl_erased(sftl->oob, sizeof(sftl->oob))) {
				ret = sftl_erased(sftl->page, APPLE_PPN_PAGE_SIZE) ? 0 : -EBADMSG;
				if (ret)
					goto out;
				gap = true;
				blank_lanes |= lane;
				continue;
			}
			if (blank_lanes & lane) {
				ret = -EBADMSG;
				goto out;
			}
			populated |= lane;
			if (gap)
				__set_bit(block, gapped);
			for (chunk = 0; chunk < 4; chunk++) {
				const u8 *meta = sftl->oob + chunk * SFTL_META;
				const u8 *payload = sftl->page + chunk * SFTL_CHUNK;

				if (sftl_erased(meta, SFTL_META)) {
					if (!sftl_erased(payload, SFTL_CHUNK)) {
						ret = -EBADMSG;
						goto out;
					}
					empty = true;
					continue;
				}
				if (empty || meta[0] != 0x1f || !meta[1] || meta[1] > 6) {
					ret = -EBADMSG;
					goto out;
				}
				if (meta[1] == 1 && !chunk) {
					ret = sftl_context_names(sftl, payload, pending, &count);
					if (ret)
						goto out;
				}
				ret = sftl_record_add(&records, sftl_stamp(meta), 0,
						      block * stride + position * 4 + chunk);
				if (ret)
					goto out;
			}
		}
		if (test_bit(block, gapped) && hweight16(populated) == lanes) {
			ret = -EBADMSG;
			goto out;
		}
	}
	ret = -EBADMSG;
	if (!records.count)
		goto out;
	sort(records.items, records.count, sizeof(*records.items), sftl_record_compare, NULL);
	/* An interrupted lane-by-lane retirement can leave older fragments
	 * without their base. They cannot provide a recovery root.
	 */
	for (oldest = 0; oldest < records.count; oldest++) {
		const struct sftl_record *r = &records.items[oldest];

		if (sftl_flags(r) == 1 && !(r->address & 3))
			break;
		if (!test_bit(r->address / stride, gapped))
			goto out;
	}
	if (oldest == records.count)
		goto out;
	for (i = 1; i < records.count; i++)
		if (!sftl_record_compare(&records.items[i - 1], &records.items[i]))
			goto out;
	for (end = records.count; end > oldest; end = first) {
		first = end - 1;
		while (first > oldest && (sftl_flags(&records.items[first]) != 1 ||
				 (records.items[first].address & 3)))
			first--;
		memset(named, 0, sizeof(named));
		ret = sftl_decode_generation(sftl, &records, first, end, named);
		if (!ret)
			break;
		if (ret != -ENODATA)
			goto out;
		sftl_clear_runs(sftl);
	}
	if (end == oldest) {
		ret = -EBADMSG;
		goto out;
	}
	sftl_mark_mapped(sftl, sftl->checkpoint_used);
	/* A native base names this save's capacity, not the previous
	 * generation. The old base may have disappeared with its first lane.
	 */
	if (sftl->native_context) {
		u32 checked = 0;
		u64 selected = records.items[first].stamp & SFTL_SEQUENCE;

		for (block = 0; block < subblocks; block++) {
			if (sftl->roles[block] != SFTL_CONTEXT)
				continue;
			if (test_bit(block, sftl->checkpoint_used) || test_bit(block, sftl->vfl_reserved)) {
				ret = -EBADMSG;
				goto out;
			}
			if (test_bit(block, sftl->unknown_lane_flags))
				sftl->context_degraded = true;
			if (test_bit(block, sftl->reserved) && !test_bit(block, gapped))
				continue;
			__set_bit(block, sftl->reserved);
			/* Bound additional full-block checks on unfamiliar media. */
			if (++checked > SFTL_MAX_CONTEXTS) {
				sftl->context_degraded = true;
				continue;
			}
			ret = sftl_check_context_remnant(sftl, block, selected);
			if (ret)
				goto out;
		}
	}
	for (i = 0; i < records.count; i++) {
		block = records.items[i].address / stride;
		if (test_bit(block, gapped)) {
			if (i >= first || (sftl->native_context ?
			    sftl->roles[block] != SFTL_CONTEXT : !test_bit(block, named))) {
				ret = -EBADMSG;
				goto out;
			}
			sftl->context_degraded = true;
		}
	}
	/* Only the selected generation can establish a data role. Canonical
	 * block tables from an older fallback can still veto initial allocation.
	 */
	for (i = 0; sftl->native_context && !sftl->context_degraded && i < first; i++) {
		const struct sftl_record *r = &records.items[i];
		const u8 *meta;
		u32 start;

		if (sftl_flags(r) != 3)
			continue;
		ret = sftl_read_address(sftl, r->address, &chunk);
		if (ret)
			goto out;
		meta = sftl->oob + chunk * SFTL_META;
		if (sftl_stamp(meta) != r->stamp) {
			ret = -EBADMSG;
			goto out;
		}
		start = get_unaligned_le16(meta + 8);
		if (start < subblocks && !(start % (SFTL_CHUNK / 8)) &&
		    apple_sftl_native_context_tag_for(subblocks, 8 + start / (SFTL_CHUNK / 8), meta))
			sftl_load_block_roles(sftl, meta, sftl->page + chunk * SFTL_CHUNK, false);
	}
	/* Retain the selected generation's opaque records for later snapshots. */
	for (i = first; i < end; i++) {
		const struct sftl_record *record = &records.items[i];

		if (sftl_flags(record) == 4)
			continue;
		ret = sftl_record_add(&sftl->context, record->stamp, record->key, record->address);
		if (ret)
			goto out;
	}
	/* Keep every possible fallback out of the erase path. */
	if (!first && end == records.count)
		sftl->checkpoint_start = records.items[0].stamp & SFTL_SEQUENCE;
	sftl->checkpoint_sequence = records.items[end - 1].stamp & SFTL_SEQUENCE;
	sftl->sequence = sftl->checkpoint_sequence;
	if (end < records.count) {
		/* Account for observed context sequences in normal journal order.
		 * Only their own IDs may bridge a gap between two data records.
		 */
		records.count -= end;
		memmove(records.items, records.items + end, records.count * sizeof(*records.items));
		*journal = records;
		records.items = NULL;
	}
	ret = 0;
out:
	kvfree(records.items);
	return ret;
}

static bool sftl_user_epoch_valid(const struct apple_sftl *sftl, const u8 *meta, u64 sequence)
{
	u32 user_epoch = get_unaligned_le32(meta + 12);

	/* Relocation restamps the weave sequence and retains the original user epoch. */
	if (meta[0] == 1 && !meta[1] && get_unaligned_le32(meta + 8) < sftl->geo->lpages)
		return user_epoch <= sequence >> 16;
	return user_epoch == sequence >> 16;
}

static bool sftl_journal_meta(const struct apple_sftl *sftl, const u8 *meta)
{
	u64 sequence = sftl_sequence(meta);

	/* Native subtype-1 data covered by the checkpoint can retain an
	 * earlier user epoch. It occupies the page but is never replayed.
	 */
	if (meta[0] == 1 && meta[1] == 1 && sequence <= sftl->checkpoint_sequence &&
	    get_unaligned_le32(meta + 8) < sftl->geo->disk_pages)
		return get_unaligned_le32(meta + 12) <= sequence >> 16;

	return sftl_erased(meta, SFTL_META) ||
		((meta[0] == 1 || meta[0] == 2) &&
		 sftl_user_epoch_valid(sftl, meta, sequence) &&
		 (meta[0] == 1 ? meta[1] <= 1 : meta[1] == 0));
}

static int sftl_add_journal(const struct apple_sftl *sftl, struct sftl_records *journal,
			    const u8 *meta, u32 address, u64 checkpoint)
{
	u64 sequence = sftl_sequence(meta);

	if (sequence <= checkpoint || sftl_erased(meta, SFTL_META))
		return 0;
	if (!sftl_journal_meta(sftl, meta))
		return -EBADMSG;
	return sftl_record_add(journal, sftl_stamp(meta), get_unaligned_le32(meta + 8), address);
}

/*
 * Cascadia's give_up_torn, at load: a program that failed or was cut off
 * can leave a page whose metadata is not a record, or an erased status
 * over programmed bytes. Only the last programmed page of an open block
 * may be such a page; what it held was never acknowledged or was written
 * again elsewhere with higher sequences. Remember where its sequences
 * began. Read errors, refresh and unknown statuses still stop recovery.
 */
static int sftl_note_torn(struct apple_sftl *sftl, u32 block, u64 after)
{
	if (test_bit(block, sftl->torn) || sftl->nr_torn == SFTL_FAIL_MAX)
		return -EBADMSG;
	__set_bit(block, sftl->torn);
	sftl->torn_after[sftl->nr_torn++] = after;
	return 0;
}

/* A torn page's four sequences may be missing after the given sequence. */
static bool sftl_take_torn(struct apple_sftl *sftl, u64 last)
{
	u32 i;

	for (i = 0; i < sftl->nr_torn; i++)
		if (sftl->torn_after[i] <= last) {
			sftl->torn_after[i] = U64_MAX;
			return true;
		}
	return false;
}

/* Read a page of an open block; one retry after a provider error. */
static int sftl_read_open(struct apple_sftl *sftl, u32 address, unsigned int *chunk)
{
	int ret = sftl_read_address(sftl, address, chunk);

	if (ret && ret != -ENOMEM && ret != -ERANGE)
		ret = sftl_read_address(sftl, address, chunk);
	return ret;
}

static int sftl_remember_user_seq(struct apple_sftl *sftl, u32 block, const u8 *footer, u32 runs)
{
	/* Four base chunks identify the layout validated by sftl_decode_generation. */
	if (sftl->context.count != APPLE_SFTL_ACCOUNT_RECORDS ||
	    sftl_flags(&sftl->context.items[3]) != 1)
		return 0;
	if (!sftl->closed_user_seq) {
		sftl->closed_user_seq = kvzalloc(SFTL_MAX_SUBBLOCKS * sizeof(*sftl->closed_user_seq),
						  GFP_NOIO);
		if (!sftl->closed_user_seq)
			return -ENOMEM;
	}
	apple_sftl_summarize_user_seq(&sftl->closed_user_seq[block], footer, runs);
	return 0;
}

static int sftl_footer(struct apple_sftl *sftl, const u8 *head, u32 block,
		       u32 pages, struct sftl_records *journal, u64 *floor)
{
	u8 footer_meta[16];
	u8 *footer;
	u32 chunks, span, index, position = 0, relative_end = 0;
	u64 base;
	unsigned int chunk;
	bool terminated = false;
	int ret;

	memcpy(footer_meta, sftl->oob, sizeof(footer_meta));
	base = sftl_sequence(footer_meta);
	chunks = get_unaligned_le16(footer_meta + 12);
	span = get_unaligned_le32(footer_meta + 8);
	if (chunks < 4 || chunks > 32 || chunks % 4 || footer_meta[1] >> 1 != chunks ||
	    get_unaligned_le16(footer_meta + 14) != 0xffff ||
	    base != sftl_sequence(head) || !span || base + span > SFTL_SEQUENCE + 1)
		return -EBADMSG;
	*floor = max(*floor, base + span - 1);
	if (base + span - 1 <= sftl->checkpoint_sequence) {
		if (head[0] == 1 && base && base + span - 1 < sftl->checkpoint_start)
			__set_bit(block, sftl->old_closed);
		return 0;
	}
	footer = kmalloc(chunks * SFTL_CHUNK, GFP_KERNEL);
	if (!footer)
		return -ENOMEM;
	for (index = 0; index < chunks / 4; index++) {
		ret = sftl_read_address(sftl, block * sftl_stride(sftl) +
					(pages - chunks / 4 + index) * 4, &chunk);
		if (ret)
			goto out;
		for (chunk = 0; chunk < 4; chunk++)
			if (memcmp(footer_meta, sftl->oob + chunk * SFTL_META, SFTL_META)) {
				ret = -EBADMSG;
				goto out;
			}
		memcpy(footer + index * APPLE_PPN_PAGE_SIZE, sftl->page, APPLE_PPN_PAGE_SIZE);
	}
	for (index = 0; index < chunks * SFTL_CHUNK; index += 16) {
		u32 relative = get_unaligned_le32(footer + index);
		u32 key = get_unaligned_le32(footer + index + 8);
		u32 length = get_unaligned_le32(footer + index + 12), offset;

		if (!length) {
			terminated = true;
			break;
		}
		if (relative < relative_end || length > U32_MAX - relative ||
		    base + relative + length > SFTL_SEQUENCE + 1 ||
		    length > pages * 4 - chunks - position) {
			ret = -EBADMSG;
			goto out;
		}
		relative_end = relative + length;
		for (offset = 0; offset < length; offset++, position++) {
			u64 sequence = base + relative + offset;
			u32 actual_key = key >= 0xffff0000 ? key : key + offset;
			u32 address = block * sftl_stride(sftl) + position;
			u8 meta[16];

			if (sequence <= sftl->checkpoint_sequence)
				continue;
			if (key < 0xffff0000 && actual_key < key) {
				ret = -EBADMSG;
				goto out;
			}
			if (actual_key == SFTL_TRIM) {
				ret = sftl_read_address(sftl, address, &chunk);
				if (ret)
					goto out;
				memcpy(meta, sftl->oob + chunk * SFTL_META, sizeof(meta));
				if (meta[0] != 2 || meta[1] || sftl_sequence(meta) != sequence ||
				    get_unaligned_le32(meta + 8) != SFTL_TRIM) {
					ret = -EBADMSG;
					goto out;
				}
			} else {
				meta[0] = 1;
				meta[1] = head[1];
				put_unaligned_le64(sequence << 16 | get_unaligned_le16(meta), meta);
				put_unaligned_le32(actual_key, meta + 8);
				if (!meta[1] && actual_key < sftl->geo->lpages)
					memcpy(meta + 12, footer + index + 4, sizeof(u32));
				else
					put_unaligned_le32(sequence >> 16, meta + 12);
			}
			ret = sftl_add_journal(sftl, journal, meta, address, sftl->checkpoint_sequence);
			if (ret)
				goto out;
		}
	}
	if (!terminated || position != pages * 4 - chunks) {
		ret = -EBADMSG;
		goto out;
	}
	/* 12H321 summaries can count their own chunks after the last run. The
	 * sequences belong to the summary and to no record of another block.
	 */
	if (sftl->geo->summary_sequences && span == relative_end + chunks) {
		u32 address = block * sftl_stride(sftl) + pages * 4 - chunks, k;

		for (k = 0; k < chunks; k++) {
			u64 sequence = base + relative_end + k;

			if (sequence <= sftl->checkpoint_sequence)
				continue;
			ret = sftl_record_add(journal, (u64)0x1c << 56 | sequence, U32_MAX,
					      address + k);
			if (ret)
				goto out;
		}
	}
	ret = sftl_remember_user_seq(sftl, block, footer, index / SFTL_META);
out:
	kfree(footer);
	return ret;
}

static bool sftl_partial_footer(struct apple_sftl *sftl, const u8 *head,
				u32 position, u32 pages, u64 span)
{
	const u8 *meta = sftl->oob;
	unsigned int chunk;

	if (position < pages - SFTL_FOOTER_CHUNKS / 4 ||
	    meta[0] != 0x1c || meta[1] != SFTL_FOOTER_CHUNKS * 2 ||
	    sftl_sequence(meta) != sftl_sequence(head) ||
	    get_unaligned_le32(meta + 8) != span ||
	    get_unaligned_le16(meta + 12) != SFTL_FOOTER_CHUNKS ||
	    get_unaligned_le16(meta + 14) != 0xffff)
		return false;
	for (chunk = 1; chunk < 4; chunk++)
		if (memcmp(meta, sftl->oob + chunk * SFTL_META, SFTL_META))
			return false;
	return true;
}

/* A Linux block may close across an observed checkpoint save. For a torn
 * suffix, admit only the exact sequence IDs actually read from context OOB.
 * The leading portion of journal is sorted until all data has been gathered.
 */
static bool sftl_context_gap(const struct apple_sftl *sftl, const struct sftl_records *journal,
			     u32 nr_context, u64 first, u64 next)
{
	u32 low = 0, high = nr_context;
	u64 count = next - first;

	if (first == (sftl->context.items[0].stamp & SFTL_SEQUENCE) &&
	    next == sftl->checkpoint_sequence + 1)
		return true;
	if (count > nr_context)
		return false;
	while (low < high) {
		u32 mid = low + (high - low) / 2;

		if ((journal->items[mid].stamp & SFTL_SEQUENCE) < first)
			low = mid + 1;
		else
			high = mid;
	}
	return count && count <= nr_context - low &&
		(journal->items[low].stamp & SFTL_SEQUENCE) == first &&
		(journal->items[low + count - 1].stamp & SFTL_SEQUENCE) == next - 1;
}

static int sftl_journal(struct apple_sftl *sftl, const u8 *heads, struct sftl_records *journal)
{
	u64 floor = sftl->checkpoint_sequence, expected = floor + 1;
	u32 nr_context = journal->count;
	u32 block, position, i, pages, stride = sftl_stride(sftl);
	unsigned int chunk;
	int ret;

	for (block = 1; block < sftl->geo->subblocks; block++) {
		const u8 *head = heads + block * SFTL_META;
		bool complete_prefix = true, partial_footer = false, torn = false;
		u64 base = sftl_sequence(head), next = base, last = 0;

		if (head[0] != 1 && head[0] != 2)
			continue;
		pages = hweight16(sftl->lanes[block]) * sftl->geo->pages;
		ret = sftl_read_open(sftl, block * stride + (pages - 1) * 4, &chunk);
		if (ret && ret != -EBADMSG)
			goto out;
		if (!ret && sftl->oob[0] == 0x1c) {
			ret = sftl_footer(sftl, head, block, pages, journal, &floor);
			if (ret)
				goto out;
			continue;
		}
		/* A full block of records needs a summary. A last page that is
		 * torn is found by the scan below.
		 */
		if (!ret && !sftl_erased(sftl->oob, sizeof(sftl->oob))) {
			for (chunk = 0; chunk < 4; chunk++)
				if (!sftl_journal_meta(sftl, sftl->oob + chunk * SFTL_META))
					break;
			if (chunk == 4) {
				ret = -EBADMSG;
				goto out;
			}
		}
		for (position = 0; position < pages; position++) {
			bool erased, records;

			ret = sftl_read_open(sftl, block * stride + position * 4, &chunk);
			if (ret && ret != -EBADMSG)
				goto out;
			erased = !ret && sftl_erased(sftl->oob, sizeof(sftl->oob));
			if (erased && !sftl_erased(sftl->page, APPLE_PPN_PAGE_SIZE))
				ret = -EBADMSG;
			records = !ret && !erased && sftl->oob[0] != 0x1c;
			for (chunk = 0; records && chunk < 4; chunk++)
				if (!sftl_journal_meta(sftl, sftl->oob + chunk * SFTL_META))
					ret = -EBADMSG;
			/* Nothing may follow a torn page in its block. */
			if (torn && (ret || !erased)) {
				ret = -EBADMSG;
				goto out;
			}
			if (ret) {
				ret = sftl_note_torn(sftl, block, last);
				if (ret)
					goto out;
				torn = true;
				complete_prefix = false;
				continue;
			}
			for (chunk = 0; records && chunk < 4; chunk++)
				last = max(last, sftl_sequence(sftl->oob + chunk * SFTL_META));
			if (position < pages - SFTL_FOOTER_CHUNKS / 4) {
				for (chunk = 0; chunk < 4; chunk++) {
					const u8 *meta = sftl->oob + chunk * SFTL_META;
					u64 sequence = sftl_sequence(meta);

					if (meta[0] != 1 || meta[1] || sequence < next ||
					    !sftl_user_epoch_valid(sftl, meta, sequence))
						complete_prefix = false;
					/* A checkpoint occupies its own sequence numbers between pages. */
					if (complete_prefix && sequence > next &&
					    (!position || chunk || !sftl_context_gap(sftl, journal,
										   nr_context, next, sequence)))
						complete_prefix = false;
					next = sequence + 1;
				}
			}
			/* A closing footer may follow a checkpoint that consumed
			 * sequences between two data pages in the same block. Match
			 * its span to the observed data, including that interval.
			 */
			if (sftl->oob[0] == 0x1c) {
				if (!complete_prefix ||
				    !sftl_partial_footer(sftl, head, position, pages,
							 next - base)) {
					ret = -EBADMSG;
					goto out;
				}
				partial_footer = true;
				continue;
			}
			if (partial_footer && !sftl_erased(sftl->oob, sizeof(sftl->oob))) {
				ret = -EBADMSG;
				goto out;
			}
			for (chunk = 0; chunk < 4; chunk++) {
				ret = sftl_add_journal(sftl, journal, sftl->oob + chunk * SFTL_META,
						       block * stride + position * 4 + chunk,
						       sftl->checkpoint_sequence);
				if (ret)
					goto out;
			}
		}
	}
	sort(journal->items, journal->count, sizeof(*journal->items), sftl_record_compare, NULL);
	for (i = 0; i < journal->count; i++) {
		const struct sftl_record *record = &journal->items[i];
		u64 sequence = record->stamp & SFTL_SEQUENCE;

		ret = -EBADMSG;
		if (sequence == expected + 4 && sftl_take_torn(sftl, expected - 1))
			expected = sequence;
		if (sequence != expected++)
			goto out;
		/* Context and summary chunks, and padding, map nothing. */
		if (sftl_kind(record) == 0x1f || sftl_kind(record) == 0x1c ||
		    record->key == SFTL_PAD)
			continue;
		if (sftl_kind(record) == 2 && record->key == SFTL_TRIM) {
			const u8 *payload;
			u32 count, offset;

			ret = sftl_read_address(sftl, record->address, &chunk);
			if (ret)
				goto out;
			ret = -EBADMSG;
			if (sftl_stamp(sftl->oob + chunk * SFTL_META) != record->stamp ||
			    get_unaligned_le32(sftl->oob + chunk * SFTL_META + 8) != record->key)
				goto out;
			payload = sftl->page + chunk * SFTL_CHUNK;
			count = get_unaligned_le32(payload);
			if (count > (SFTL_CHUNK - 4) / 8)
				goto out;
			for (offset = 0; offset < count; offset++) {
				u32 start = get_unaligned_le32(payload + 4 + offset * 8);
				u32 length = get_unaligned_le32(payload + 8 + offset * 8);

				if (!length)
					continue;
				ret = sftl_replace(sftl, start, length, SFTL_UNMAPPED);
				if (ret)
					goto out;
			}
		} else {
			ret = sftl_replace(sftl, record->key, 1, record->address);
			if (ret)
				goto out;
		}
		sftl->sequence = sequence;
	}
	sftl->journal_sequence = expected - 1;
	/* A torn page after the last record keeps its sequences reserved. */
	if (sftl_take_torn(sftl, sftl->journal_sequence) && floor <= sftl->journal_sequence + 4)
		sftl->journal_sequence += 4;
	sftl->sequence = max(sftl->journal_sequence, floor);
	ret = 0;
out:
	return ret;
}

static void sftl_mark_mapped(struct apple_sftl *sftl, unsigned long *blocks)
{
	struct rb_node *node;
	u32 block;

	for (node = rb_first(&sftl->runs); node; node = rb_next(node)) {
		struct sftl_run *run = rb_entry(node, struct sftl_run, node);
		u32 end;

		if (run->provider == SFTL_UNMAPPED)
			continue;
		end = (run->provider + run->end - run->start - 1) / sftl_stride(sftl);
		for (block = run->provider / sftl_stride(sftl); block <= end; block++)
			__set_bit(block, blocks);
	}
}

int apple_sftl_load(struct apple_ppn *provider, struct apple_sftl **out)
{
	struct apple_sftl *sftl;
	const struct sftl_geometry *geo = NULL;
	struct sftl_records journal = {};
	u8 *heads;
	u32 block;
	unsigned int chunk;
	int ret;

	if (!out)
		return -EINVAL;
	*out = NULL;
	if (!provider || !provider->ops || !provider->ops->read_page)
		return -EOPNOTSUPP;
	for (block = 0; block < ARRAY_SIZE(sftl_geometries); block++) {
		const struct sftl_geometry *g = sftl_geometries[block];

		if (provider->buses == g->buses && provider->caus == g->caus &&
		    provider->blocks == g->blocks && provider->pages == g->pages)
			geo = g;
	}
	if (!geo)
		return -EOPNOTSUPP;
	sftl = kzalloc(sizeof(*sftl), GFP_KERNEL);
	if (!sftl)
		return -ENOMEM;
	sftl->ppn = provider;
	sftl->geo = geo;
	sftl->runs = RB_ROOT;
	sftl->append_block = U32_MAX;
	sftl->next_candidate = 1;
	sftl->spare = U32_MAX;
	mutex_init(&sftl->lock);
	sftl->page = kmalloc(APPLE_PPN_PAGE_SIZE, GFP_KERNEL);
	heads = kcalloc(geo->blocks, SFTL_META, GFP_KERNEL);
	if (!sftl->page || !heads) {
		ret = -ENOMEM;
		goto failed;
	}
	memset(heads, 0xff, geo->blocks * SFTL_META);
	ret = sftl_vfl(sftl);
	if (ret)
		goto failed;
	for (block = 1; block < geo->subblocks; block++) {
		if (!sftl->lanes[block])
			continue;
		ret = sftl_read_open(sftl, block * sftl_stride(sftl), &chunk);
		if (ret && ret != -EBADMSG)
			goto failed;
		/* A torn first page is scanned as an open block with no records. */
		if (ret || (sftl_erased(sftl->oob, sizeof(sftl->oob)) &&
			    !sftl_erased(sftl->page, APPLE_PPN_PAGE_SIZE))) {
			memset(heads + block * SFTL_META, 0, SFTL_META);
			heads[block * SFTL_META] = 1;
			continue;
		}
		if (sftl_erased(sftl->oob, sizeof(sftl->oob))) {
			__set_bit(block, sftl->candidates);
		} else if (sftl->oob[0] == 0x1f || sftl->oob[0] == 1 || sftl->oob[0] == 2) {
			memcpy(heads + block * SFTL_META, sftl->oob, SFTL_META);
		} else {
			ret = -EOPNOTSUPP;
			goto failed;
		}
	}
	ret = sftl_context(sftl, heads, &journal);
	if (ret)
		goto failed;
	/* A map entry superseded by journal replay still belongs to the saved map. */
	ret = sftl_journal(sftl, heads, &journal);
	if (ret)
		goto failed;
	kvfree(journal.items);
	kfree(heads);
	*out = sftl;
	return 0;
failed:
	kvfree(journal.items);
	kfree(heads);
	apple_sftl_free(sftl);
	return ret;
}
EXPORT_SYMBOL_GPL(apple_sftl_load);

void apple_sftl_free(struct apple_sftl *sftl)
{
	if (!sftl)
		return;
	sftl_clear_runs(sftl);
	kvfree(sftl->footer);
	kvfree(sftl->stage);
	kvfree(sftl->move);
	kvfree(sftl->context.items);
	kvfree(sftl->closed_user_seq);
	kfree(sftl->page);
	kfree(sftl);
}
EXPORT_SYMBOL_GPL(apple_sftl_free);

int apple_sftl_read(struct apple_sftl *sftl, u32 logical_page, u8 *data)
{
	struct sftl_run *run;
	unsigned int chunk;
	u32 address;
	const u8 *meta;
	int ret = 0;

	if (!sftl || !data || logical_page >= sftl->geo->lpages)
		return -EINVAL;
	mutex_lock(&sftl->lock);
	if (sftl->failed) {
		ret = -EIO;
		goto out;
	}
	run = sftl_find(sftl, logical_page);
	if (!run) {
		ret = -EBADMSG;
		goto failed;
	}
	if (run->provider == SFTL_UNMAPPED) {
		memset(data, 0, SFTL_CHUNK);
		goto out;
	}
	address = run->provider + logical_page - run->start;
	ret = sftl_read_address(sftl, address, &chunk);
	if (ret)
		goto failed;
	meta = sftl->oob + chunk * SFTL_META;
	if ((meta[0] != 1 && meta[0] != 2) || get_unaligned_le32(meta + 8) != logical_page) {
		ret = -EBADMSG;
		goto failed;
	}
	memcpy(data, sftl->page + chunk * SFTL_CHUNK, SFTL_CHUNK);
	goto out;
failed:
	sftl->failed = true;
out:
	mutex_unlock(&sftl->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(apple_sftl_read);

void apple_sftl_get_info(struct apple_sftl *sftl, struct apple_sftl_info *info)
{
	info->read_only = sftl->vfl_degraded || sftl->context_degraded || !sftl->geo->writer;
	info->logical_pages = sftl->geo->lpages;
	info->disk_pages = sftl->geo->disk_pages;
	info->nr_runs = sftl->nr_runs;
	info->checkpoint_sequence = sftl->checkpoint_sequence;
	info->sequence = sftl->sequence;
	info->journal_sequence = sftl->journal_sequence;
}
