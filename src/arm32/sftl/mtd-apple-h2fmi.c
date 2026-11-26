// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple S5L H2FMI + Hynix PPN NAND.
 *
 * The iPad mini 1 has two PPN packages, one on CE0 of each of the SoC's two
 * flash buses.  "PPN" means the package has its own controller: it does the
 * ECC and part of the bad-block work itself, so this is a *managed* NAND and
 * deliberately NOT a driver on top of drivers/mtd/nand/raw -- that framework
 * assumes the host computes ECC and would have to be fought the whole way.
 * What it is instead is a plain mtd_info per die with its own _read.
 *
 * Every register use and every command sequence here comes from iBEC's own
 * disassembly (H2fmi.c, H2fmi_ppn.c, fmiss_ppn.c) by way of the userspace tool
 * this is a port of, tools/nand/ppn.c, which read the whole NAND bit-for-bit
 * repeatably.  docs/research/p105-nand.md is the notebook.  When iBoot did not
 * leave the interface in DDR, the SDR bring-up and transfer shapes follow
 * Project Cascadia's PIO driver (apple_ppn.c) and its specification.
 *
 * Geometry is read from each PPN package before registering any MTDs.
 * The supported packages have one CE per bus and either two or eight CAUs.
 * A page has 16448 bytes: four chunks of 1024 data bytes, 16 metadata
 * bytes and 3072 more data bytes.  The metadata is the FTL's, and becomes MTD's OOB; strip it and a
 * page is 16 KiB of data with 64 bytes of OOB. One MTD is exported per
 * CAU. Raw MTD offsets are independent of the iOS logical block addresses.
 *
 * Pages are read and programmed whole, data and metadata together.  The
 * raw MTD devices provide no erase. A page is only programmed when it
 * reads back erased, and every program is read back and compared.  After a
 * failed transaction the packages are reset before the next one.
 * Logical-address translation and allocation belong to the FTL above.
 */


/*
 * m1n1: the read half of Orchard's Linux driver (drivers/mtd/devices/
 * mtd-apple-h2fmi.c), unchanged, between the two markers below. What Linux
 * gets from its driver model comes from include/linux/kernel.h and the glue
 * at the end of this file instead. m1n1 never programs or erases flash.
 */

#include <linux/kernel.h>
#include <linux/mtd/apple-ppn.h>

#include "../../adt.h"
#include "apple-sftl.h"
#include "mtd-apple-h2fmi.h"

/* ---- from the Linux driver ---- */

#define H2FMI_BUSES		2
#define H2FMI_PAGE_MASK		(H2FMI_PAGE_DATA - 1)

/* The ADT's ce-bitmap is 0x101: CE0 on each bus, and nothing else.  Addressing
 * a CE with no chip behind it wedges the FMC in DDR -- it waits for a DQS that
 * never comes -- so this is never computed, only ever CE0. */
#define H2FMI_CE			0

/* FMI: the DMA/PIO side.  FMC: the NAND bus.  Offsets as iBEC uses them. */
#define FMI_CONFIG		0x00
#define FMI_CONTROL		0x04
#define FMI_STATUS		0x0c
#define FMI_INTEN		0x10
#define FMI_DATA		0x14	/* PIO FIFO; a read pops it */
#define FMI_DMA_STATUS		0x1c
#define FMI_PIO_CONFIG		0x34

#define FMC_ON			0x00
#define FMC_IF_CTRL		0x08
#define FMC_CE_CTRL		0x0c
#define FMC_RW_CTRL		0x10
#define FMC_CMD			0x14
#define FMC_ADDR0		0x18
#define FMC_ADDRNUM		0x20
#define FMC_DATANUM		0x24
#define FMC_INTMASK		0x40
#define FMC_STATUS		0x44
#define FMC_NAND_STATUS		0x48
#define FMC_STATUS_MASK		0x4c
#define FMC_ADDR1		0x1c

#define ECC_INIT		0x14	/* FMI + 0x80014 */
#define FMI_ALL_EVENTS		0x10f
#define FMI_POLL_DONE		BIT(8)
#define FMC_ON_SDR		1
#define FMC_ON_DDR		5

/* Every command sequence the driver issues.  A sequence that is not listed
 * does not run. Only the single-row final erase command is needed. */
static const u8 h2fmi_ops[][2] = {
	{ 0x70, 0x00 },		/* read status */
	{ 0x92, 0x97 },		/* PPN device parameters */
	{ 0x77, 0x7d },		/* PPN operation status */
	{ 0x77, 0x00 },		/* PPN end of operation */
	{ 0x7a, 0x00 },		/* PPN data out */
	{ 0x0a, 0x37 },		/* PPN page read */
	{ 0x8a, 0x00 },		/* PPN program, address phase */
	{ 0x17, 0x00 },		/* PPN program confirm */
	{ 0x79, 0x00 },		/* PPN mutation status */
	{ 0x6a, 0xd7 },		/* PPN erase, one normal row */
	{ 0xff, 0x00 },		/* NAND reset (SDR bring-up) */
	{ 0x90, 0x00 },		/* read ID (SDR bring-up) */
	{ 0xee, 0xe7 },		/* PPN get feature */
	{ 0xef, 0x00 },		/* PPN set feature, address phase */
	{ 0xe7, 0x00 },		/* PPN set feature confirm */
};

struct h2fmi_bus {
	void __iomem *fmi;
	void __iomem *fmc;
	void __iomem *ecc;
	struct mutex lock;	/* one command sequence on this bus at a time */
	u8 *page;		/* one raw page; only touched under lock */
	struct h2fmi_geometry geometry;
	u32 if_ctrl;		/* SDR timing restored by every software reset */
	bool sdr;		/* this driver reset the package to SDR */
	bool failed;		/* block further I/O in this driver instance */
};

/* Resets after failed transactions in one driver instance. */
#define H2FMI_MAX_RESETS	8

struct h2fmi {
	struct device *dev;
	struct h2fmi_bus bus[H2FMI_BUSES];
	bool reset_needed;	/* a transaction failed; reset before the next */
	unsigned int resets;
};

/*
 * h2fmi_wait_done: poll until (reg & mask) == want, then write want back --
 * these bits are write-one-to-clear.  iBEC gives up after 100 ms and so do we.
 */
static int h2fmi_wait(struct h2fmi *fmi, void __iomem *base, u32 off,
		      u32 mask, u32 want)
{
	u32 v;
	int ret;

	ret = readl_poll_timeout(base + off, v, (v & mask) == want, 0, 100000);
	if (ret) {
		dev_err(fmi->dev, "timeout: reg +0x%x = 0x%08x, wanted 0x%x/0x%x\n",
			off, v, mask, want);
		return ret;
	}
	writel(want, base + off);
	return 0;
}

/*
 * FMC_CMD holds the first command byte, and the second one in bits 15:8.
 * Every command sequence passes through the opcode list above.
 */
static int h2fmi_cmd_write(struct h2fmi *fmi, struct h2fmi_bus *b, u8 op, u8 op2)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(h2fmi_ops); i++)
		if (h2fmi_ops[i][0] == op && h2fmi_ops[i][1] == op2) {
			writel(op | op2 << 8, b->fmc + FMC_CMD);
			return 0;
		}

	dev_err(fmi->dev, "refusing NAND opcodes %02x %02x\n", op, op2);
	return -EPERM;
}

static void h2fmi_clear_irqs(struct h2fmi_bus *b)
{
	writel(0, b->fmc + FMC_INTMASK);
	writel(0, b->fmi + FMI_INTEN);
	writel(0x31ffff, b->fmc + FMC_STATUS);
	writel(0xf, b->fmi + FMI_STATUS);
}

/* single command, FMC_RW_CTRL bit 0 */
static int h2fmi_cmd1(struct h2fmi *fmi, struct h2fmi_bus *b, u8 op)
{
	int ret = h2fmi_cmd_write(fmi, b, op, 0);

	if (ret)
		return ret;
	writel(1, b->fmc + FMC_RW_CTRL);
	return h2fmi_wait(fmi, b->fmc, FMC_STATUS, 1, 1);
}

/* command, address bytes, second command: RW_CTRL 0xb */
static int h2fmi_cmd_addr_cmd(struct h2fmi *fmi, struct h2fmi_bus *b,
			      u8 op, u8 op2, u32 addr, int naddr)
{
	int ret;

	if (naddr < 1 || naddr > sizeof(addr))
		return -EINVAL;
	ret = h2fmi_cmd_write(fmi, b, op, op2);
	if (ret)
		return ret;
	writel(addr, b->fmc + FMC_ADDR0);
	writel((naddr + 7) & 7, b->fmc + FMC_ADDRNUM);
	writel(0xb, b->fmc + FMC_RW_CTRL);
	return h2fmi_wait(fmi, b->fmc, FMC_STATUS, 0xb, 0xb);
}

/*
 * h2fmi_ppn_get_operation_status + h2fmi_get_nand_status: 77 7D, then let the
 * FMC poll the status byte until it is ready.  iBEC waits on the interrupt;
 * this polls FMC_STATUS bit 5, as the userspace tool does.
 */
static int h2fmi_poll_status(struct h2fmi *fmi, struct h2fmi_bus *b, u8 *st)
{
	int ret;
	u32 v;

	h2fmi_clear_irqs(b);
	writel(0x100, b->fmi + FMI_INTEN);
	writel(readl(b->fmc + FMC_IF_CTRL) & ~0x100000u, b->fmc + FMC_IF_CTRL);
	writel(0x4040, b->fmc + FMC_STATUS_MASK);
	writel(0, b->fmc + FMC_DATANUM);
	writel(0x20, b->fmc + FMC_INTMASK);
	writel(0x50, b->fmc + FMC_RW_CTRL);

	ret = readl_poll_timeout(b->fmc + FMC_STATUS, v, v & 0x20, 0, 2000000);
	if (ret) {
		dev_err(fmi->dev, "status timeout: FMC+0x44 0x%08x FMI+0xc 0x%08x byte 0x%02x\n",
			v, readl(b->fmi + FMI_STATUS),
			(u8)readl(b->fmc + FMC_NAND_STATUS));
		writel(0, b->fmc + FMC_RW_CTRL);
		h2fmi_clear_irqs(b);
		return ret;
	}

	*st = readl(b->fmc + FMC_NAND_STATUS);
	writel(0, b->fmc + FMC_RW_CTRL);
	h2fmi_clear_irqs(b);
	return 0;
}

static int h2fmi_status(struct h2fmi *fmi, struct h2fmi_bus *b, u8 *st)
{
	int ret = h2fmi_cmd_write(fmi, b, 0x77, 0x7d);

	if (ret)
		return ret;
	writel(3, b->fmc + FMC_RW_CTRL);
	ret = h2fmi_wait(fmi, b->fmc, FMC_STATUS, 3, 3);
	if (ret)
		return ret;
	return h2fmi_poll_status(fmi, b, st);
}

static int h2fmi_pio_read(struct h2fmi *fmi, struct h2fmi_bus *b,
			  u32 *dst, u32 bytes)
{
	u32 v;
	int ret;
	u32 i;

	ret = readl_poll_timeout(b->fmi + FMI_DMA_STATUS, v, v & 0x18, 0, 100000);
	if (ret) {
		dev_err(fmi->dev, "PIO timeout: FMI+0x1c = 0x%08x\n", v);
		return ret;
	}
	for (i = 0; i < bytes / 4; i++)
		dst[i] = readl(b->fmi + FMI_DATA);
	return 0;
}

/*
 * h2fmi_ppn_read_data_out: 7A, then the bytes by PIO in runs of at most 0x400.
 * FMI_PIO_CONFIG is (bytes << 8) | sectors.
 */
static int h2fmi_data_out(struct h2fmi *fmi, struct h2fmi_bus *b,
			  u8 *buf, u32 len)
{
	u32 off;
	int ret;

	ret = h2fmi_cmd1(fmi, b, 0x7a);
	if (ret)
		return ret;

	for (off = 0; off < len; ) {
		u32 n = min_t(u32, len - off, 0x400);
		u32 nw = n < 4 ? 4 : n & ~3u;

		writel(5, b->fmi + FMI_CONFIG);
		writel((nw << 8 & 0x7fc00) | 1, b->fmi + FMI_PIO_CONFIG);
		writel(3, b->fmi + FMI_CONTROL);
		ret = h2fmi_pio_read(fmi, b, (u32 *)(buf + off), nw);
		if (ret)
			return ret;
		off += nw;
	}
	return 0;
}

/*
 * h2fmi_device_reset without the PMGR pulse: stop the FMI and put the FMC back
 * the way it was.  iBoot leaves it in DDR (FMC_ON = 5) and that has to survive
 * every sequence, because getting back to DDR afterwards needs a reboot.
 */
static void h2fmi_device_reset(struct h2fmi_bus *b, u32 on, u32 if_ctrl)
{
	writel(6, b->fmi + FMI_CONTROL);
	writel(on, b->fmc + FMC_ON);
	writel(if_ctrl, b->fmc + FMC_IF_CTRL);
}

/*
 * SDR operation. Cascadia's driver resets the package and runs it in SDR,
 * where every operation starts and ends with a software reset of the bus,
 * a status poll completes on FMI status bit 8 and a page moves as four
 * units of 4112 bytes, each one start of four 1028-byte sectors. A page read
 * sends the full address and clocks the page out unless the package reports
 * a general error.
 */
static void h2fmi_sdr_reset(struct h2fmi_bus *b)
{
	writel(6, b->fmi + FMI_CONTROL);
	writel(FMC_ON_SDR, b->fmc + FMC_ON);
	writel(b->if_ctrl, b->fmc + FMC_IF_CTRL);
}

static void h2fmi_sdr_clear(struct h2fmi_bus *b)
{
	writel(0, b->fmc + FMC_INTMASK);
	writel(0, b->fmi + FMI_INTEN);
	writel(0x31ffff, b->fmc + FMC_STATUS);
	writel(FMI_ALL_EVENTS, b->fmi + FMI_STATUS);
}

static int h2fmi_sdr_poll(struct h2fmi *fmi, struct h2fmi_bus *b, u8 *status)
{
	ktime_t end = ktime_add_us(ktime_get(), 2000000);

	h2fmi_sdr_clear(b);
	writel(b->if_ctrl & ~0x100000u, b->fmc + FMC_IF_CTRL);
	writel(0x4040, b->fmc + FMC_STATUS_MASK);
	writel(0, b->fmc + FMC_DATANUM);
	writel(0x20, b->fmc + FMC_INTMASK);
	writel(0x50, b->fmc + FMC_RW_CTRL);
	while (!(readl(b->fmi + FMI_STATUS) & FMI_POLL_DONE)) {
		if (ktime_after(ktime_get(), end)) {
			writel(0, b->fmc + FMC_RW_CTRL);
			writel(b->if_ctrl, b->fmc + FMC_IF_CTRL);
			dev_err(fmi->dev, "SDR status timeout\n");
			return -ETIMEDOUT;
		}
		cpu_relax();
	}
	*status = readl(b->fmc + FMC_NAND_STATUS);
	writel(FMI_POLL_DONE, b->fmi + FMI_STATUS);
	writel(0, b->fmc + FMC_RW_CTRL);
	writel(b->if_ctrl, b->fmc + FMC_IF_CTRL);
	return 0;
}

/* Operation status (77 7D) and its poll. */
static int h2fmi_sdr_status(struct h2fmi *fmi, struct h2fmi_bus *b, u8 *status)
{
	int ret = h2fmi_cmd_write(fmi, b, 0x77, 0x7d);

	if (ret)
		return ret;
	writel(3, b->fmc + FMC_RW_CTRL);
	ret = h2fmi_wait(fmi, b->fmc, FMC_STATUS, 3, 3);
	return ret ? ret : h2fmi_sdr_poll(fmi, b, status);
}

static int h2fmi_sdr_fifo(struct h2fmi *fmi, struct h2fmi_bus *b)
{
	u32 v;
	int ret;

	ret = readl_poll_timeout(b->fmi + FMI_DMA_STATUS, v, v & 0x18, 0, 100000);
	if (ret)
		dev_err(fmi->dev, "SDR PIO timeout: FMI+0x1c = 0x%08x\n", v);
	return ret;
}

/* One start per 4112-byte unit: four sectors of 1028 bytes. */
static void h2fmi_sdr_unit_start(struct h2fmi_bus *b, bool transmit)
{
	h2fmi_sdr_clear(b);
	writel(5, b->fmi + FMI_CONFIG);
	writel(H2FMI_SDR_SECTOR << 8 | 4, b->fmi + FMI_PIO_CONFIG);
	writel(transmit ? 5 : 3, b->fmi + FMI_CONTROL);
}

/* The units arrive in the stream order of the raw page. */
static int h2fmi_sdr_receive(struct h2fmi *fmi, struct h2fmi_bus *b, u8 *raw)
{
	unsigned int sector, word;
	int ret;

	for (sector = 0; sector < H2FMI_PAGE_RAW / H2FMI_SDR_SECTOR; sector++) {
		u8 *p = raw + sector * H2FMI_SDR_SECTOR;

		if (!(sector % 4))
			h2fmi_sdr_unit_start(b, false);
		ret = h2fmi_sdr_fifo(fmi, b);
		if (ret)
			return ret;
		for (word = 0; word < H2FMI_SDR_SECTOR; word += 4)
			put_unaligned_le32(readl(b->fmi + FMI_DATA), p + word);
	}
	return 0;
}

/* Transmit mirrors the receive; each unit ends with the done bit. */
static int h2fmi_sdr_transmit(struct h2fmi *fmi, struct h2fmi_bus *b, const u8 *raw)
{
	unsigned int sector, word;
	int ret;

	for (sector = 0; sector < H2FMI_PAGE_RAW / H2FMI_SDR_SECTOR; sector++) {
		const u8 *p = raw + sector * H2FMI_SDR_SECTOR;

		if (!(sector % 4))
			h2fmi_sdr_unit_start(b, true);
		ret = h2fmi_sdr_fifo(fmi, b);
		if (ret)
			return ret;
		for (word = 0; word < H2FMI_SDR_SECTOR; word += 4)
			writel(get_unaligned_le32(p + word), b->fmi + FMI_DATA);
		if (sector % 4 == 3) {
			ret = h2fmi_wait(fmi, b->fmi, FMI_STATUS, 2, 2);
			if (ret)
				return ret;
		}
	}
	return 0;
}

/* One transfer of at most 1024 bytes, as feature data moves. */
static int h2fmi_sdr_burst(struct h2fmi *fmi, struct h2fmi_bus *b, u8 *buf, u32 bytes,
			   bool transmit)
{
	u32 word;
	int ret;

	writel(5, b->fmi + FMI_CONFIG);
	writel((bytes << 8 & 0x7fc00) | 1, b->fmi + FMI_PIO_CONFIG);
	writel(transmit ? 5 : 3, b->fmi + FMI_CONTROL);
	ret = h2fmi_sdr_fifo(fmi, b);
	if (ret)
		return ret;
	for (word = 0; word < bytes; word += 4) {
		if (transmit)
			writel(get_unaligned_le32(buf + word), b->fmi + FMI_DATA);
		else
			put_unaligned_le32(readl(b->fmi + FMI_DATA), buf + word);
	}
	return transmit ? h2fmi_wait(fmi, b->fmi, FMI_STATUS, 2, 2) : 0;
}

static int h2fmi_cmd_addr(struct h2fmi *fmi, struct h2fmi_bus *b, u8 op, u32 addr, int naddr)
{
	int ret = h2fmi_cmd_write(fmi, b, op, 0);

	if (ret)
		return ret;
	writel(addr, b->fmc + FMC_ADDR0);
	writel((naddr + 7) & 7, b->fmc + FMC_ADDRNUM);
	writel(9, b->fmc + FMC_RW_CTRL);
	return h2fmi_wait(fmi, b->fmc, FMC_STATUS, 9, 9);
}

/* Parameters (92 97) or a feature (EE E7): status, 7A, data, 77. A tail
 * shorter than a word moves as one more 4-byte transfer.
 */
static int h2fmi_sdr_get(struct h2fmi *fmi, struct h2fmi_bus *b, u8 op, u8 op2, u32 address,
			 int naddr, u8 *buf, u32 length)
{
	u32 done = 0;
	u8 status = 0;
	int ret;

	writel(1u << H2FMI_CE, b->fmc + FMC_CE_CTRL);
	h2fmi_sdr_clear(b);
	ret = h2fmi_cmd_addr_cmd(fmi, b, op, op2, address, naddr);
	if (!ret)
		ret = h2fmi_sdr_status(fmi, b, &status);
	if (!ret && status != PPN_ST_GOOD)
		ret = -EIO;
	if (!ret)
		ret = h2fmi_cmd1(fmi, b, 0x7a);
	while (!ret && done < length) {
		u32 bytes = min_t(u32, length - done, 0x400);
		u8 tail[4];

		if (bytes >= 4) {
			bytes &= ~3u;
			ret = h2fmi_sdr_burst(fmi, b, buf + done, bytes, false);
		} else {
			ret = h2fmi_sdr_burst(fmi, b, tail, 4, false);
			memcpy(buf + done, tail, bytes);
		}
		done += bytes;
	}
	writel(6, b->fmi + FMI_CONTROL);
	if (!ret)
		ret = h2fmi_cmd1(fmi, b, 0x77);
	writel(0, b->fmc + FMC_CE_CTRL);
	return ret;
}

/* Set feature: EF and two address bytes, one word out, E7, status, 77. */
static int h2fmi_sdr_set(struct h2fmi *fmi, struct h2fmi_bus *b, u16 feature, u32 value)
{
	u8 word[4], status = 0;
	int ret;

	put_unaligned_le32(value, word);
	writel(1u << H2FMI_CE, b->fmc + FMC_CE_CTRL);
	writel(6, b->fmi + FMI_CONTROL);
	ret = h2fmi_cmd_addr(fmi, b, 0xef, feature, 2);
	if (!ret) {
		h2fmi_sdr_clear(b);
		ret = h2fmi_sdr_burst(fmi, b, word, sizeof(word), true);
	}
	if (!ret)
		ret = h2fmi_cmd1(fmi, b, 0xe7);
	if (!ret)
		ret = h2fmi_sdr_status(fmi, b, &status);
	if (!ret && status != PPN_ST_GOOD)
		ret = -EIO;
	if (!ret)
		ret = h2fmi_cmd1(fmi, b, 0x77);
	writel(6, b->fmi + FMI_CONTROL);
	writel(0, b->fmc + FMC_CE_CTRL);
	h2fmi_sdr_reset(b);
	return ret;
}

static int h2fmi_sdr_id(struct h2fmi *fmi, struct h2fmi_bus *b, u8 *id)
{
	u32 v;
	int ret;

	writel(1u << H2FMI_CE, b->fmc + FMC_CE_CTRL);
	h2fmi_sdr_clear(b);
	ret = h2fmi_cmd_addr(fmi, b, 0x90, 0, 1);
	if (!ret) {
		writel(0, b->fmi + FMI_CONFIG);
		writel(H2FMI_ID_SIZE << 8 | 1, b->fmi + FMI_PIO_CONFIG);
		writel(3, b->fmi + FMI_CONTROL);
		ret = readl_poll_timeout(b->fmi + FMI_STATUS, v, v & 2, 0, 100000);
	}
	if (!ret) {
		put_unaligned_le32(readl(b->fmi + FMI_DATA), id);
		put_unaligned_le32(readl(b->fmi + FMI_DATA), id + 4);
	}
	writel(0, b->fmc + FMC_CE_CTRL);
	h2fmi_sdr_reset(b);
	return ret;
}

/*
 * Bring both packages up in SDR in the order Cascadia's driver read the
 * whole 16 GB device with: software resets, the ECC window's init register,
 * a NAND reset of the populated CE at the low-power timing, read ID, the
 * parameters, the asynchronous power state on each CE once both are reset
 * (without it every array read fails), the working timing, the firmware
 * version and every CAU's bad-block table. Caller holds both bus locks.
 * After a failure, expect is the geometry already in use.
 */
static int h2fmi_sdr_bringup(struct h2fmi *fmi, const struct h2fmi_geometry *expect)
{
	struct h2fmi_geometry g;
	u8 id[H2FMI_ID_SIZE], firmware[H2FMI_FIRMWARE_SIZE];
	unsigned int i, cau;
	int ret;

	for (i = 0; i < H2FMI_BUSES; i++) {
		struct h2fmi_bus *b = &fmi->bus[i];

		b->sdr = true;
		b->if_ctrl = 0;
		h2fmi_sdr_reset(b);
		writel(0, b->ecc + ECC_INIT);
	}
	for (i = 0; i < H2FMI_BUSES; i++) {
		struct h2fmi_bus *b = &fmi->bus[i];

		writel(1u << H2FMI_CE, b->fmc + FMC_CE_CTRL);
		udelay(10);
		ret = h2fmi_cmd1(fmi, b, 0xff);
		writel(0, b->fmc + FMC_CE_CTRL);
		if (ret)
			return ret;
		msleep(50);
		b->if_ctrl = H2FMI_IF_CTRL_LOW_POWER;
		writel(b->if_ctrl, b->fmc + FMC_IF_CTRL);
		ret = h2fmi_sdr_id(fmi, b, id);
		if (ret)
			return ret;
		if (!h2fmi_ppn_id(id)) {
			dev_err(fmi->dev, "bus %u: ID %*ph is not PPN 1.5\n", i, H2FMI_ID_SIZE, id);
			return -ENODEV;
		}
	}
	ret = h2fmi_sdr_get(fmi, &fmi->bus[0], 0x92, 0x97, 0, 1, fmi->bus[0].page,
			    H2FMI_PARAMS_SIZE);
	if (!ret)
		ret = h2fmi_parse_geometry(fmi->bus[0].page, H2FMI_PARAMS_SIZE, &g);
	if (ret)
		return ret;
	/* After a failure, the parameters must describe the package in use. */
	if (expect && memcmp(expect, &g, sizeof(g)))
		return -ENODEV;
	for (i = 0; i < H2FMI_BUSES; i++) {
		ret = h2fmi_sdr_set(fmi, &fmi->bus[i], H2FMI_FEATURE_POWER, H2FMI_POWER_ASYNC);
		if (ret) {
			dev_err(fmi->dev, "bus %u: power state: %d\n", i, ret);
			return ret;
		}
	}
	for (i = 0; i < H2FMI_BUSES; i++) {
		fmi->bus[i].if_ctrl = H2FMI_IF_CTRL_SDR;
		writel(H2FMI_IF_CTRL_SDR, fmi->bus[i].fmc + FMC_IF_CTRL);
	}
	ret = h2fmi_sdr_get(fmi, &fmi->bus[0], 0xee, 0xe7, H2FMI_FEATURE_FIRMWARE, 2,
			    firmware, sizeof(firmware));
	if (ret)
		return ret;
	for (i = 0; i < H2FMI_BUSES; i++)
		for (cau = 0; cau < g.caus; cau++) {
			ret = h2fmi_sdr_get(fmi, &fmi->bus[i], 0xee, 0xe7,
					    h2fmi_bad_block_feature(cau), 2, fmi->bus[i].page,
					    h2fmi_bad_block_bytes(&g));
			if (ret) {
				dev_err(fmi->dev, "bus %u CAU %u: bad-block table: %d\n",
					i, cau, ret);
				return ret;
			}
		}
	dev_info(fmi->dev, "PPN %*ph, firmware %*pE, SDR\n", H2FMI_ID_SIZE, id,
		 H2FMI_FIRMWARE_SIZE, firmware);
	return 0;
}

/*
 * A failed transaction leaves the package state unknown. iBEC and Cascadia
 * recover by resetting the packages and running the bring-up again; the
 * failed operation itself is still reported. Caller holds b->lock.
 */
static void h2fmi_fail(struct h2fmi *fmi)
{
	WRITE_ONCE(fmi->reset_needed, true);
}

static int h2fmi_recover(struct h2fmi *fmi)
{
	unsigned int i;
	int ret = 0;

	mutex_lock(&fmi->bus[0].lock);
	mutex_lock_nested(&fmi->bus[1].lock, SINGLE_DEPTH_NESTING);
	if (fmi->bus[0].failed || fmi->bus[1].failed) {
		ret = -EIO;
	} else if (fmi->reset_needed) {
		ret = fmi->resets++ < H2FMI_MAX_RESETS ?
		      h2fmi_sdr_bringup(fmi, &fmi->bus[0].geometry) : -EIO;
		for (i = 0; ret && i < H2FMI_BUSES; i++)
			fmi->bus[i].failed = true;
		if (ret)
			dev_err(fmi->dev, "PPN reset %u failed (%d), stopping\n", fmi->resets, ret);
		else
			dev_warn(fmi->dev, "PPN packages reset after a failed transaction (%u)\n",
				 fmi->resets);
		fmi->reset_needed = false;
	}
	mutex_unlock(&fmi->bus[1].lock);
	mutex_unlock(&fmi->bus[0].lock);
	return ret;
}

/* Take a bus for one transaction, resetting the packages first if needed. */
static int h2fmi_lock(struct h2fmi *fmi, struct h2fmi_bus *b)
{
	if (READ_ONCE(fmi->reset_needed)) {
		int ret = h2fmi_recover(fmi);

		if (ret)
			return ret;
	}
	mutex_lock(&b->lock);
	return 0;
}

/* Caller holds b->lock. A page read in SDR, or the parameters. */
static int h2fmi_sdr_read(struct h2fmi *fmi, struct h2fmi_bus *b, u8 op, u32 row,
			  u8 *buffer, u32 length, u8 *status)
{
	u32 addr0, addr1;
	int ret, end_ret = 0;

	if (op == 0x92) {
		h2fmi_sdr_reset(b);
		ret = h2fmi_sdr_get(fmi, b, 0x92, 0x97, 0, 1, buffer, length);
		*status = ret ? 0 : PPN_ST_GOOD;
		h2fmi_sdr_reset(b);
		return ret;
	}
	if (length != H2FMI_PAGE_RAW)
		return -EINVAL;
	h2fmi_full_address(row, &addr0, &addr1);
	h2fmi_sdr_reset(b);
	writel(1u << H2FMI_CE, b->fmc + FMC_CE_CTRL);
	h2fmi_sdr_clear(b);
	ret = h2fmi_cmd_write(fmi, b, 0x0a, 0x37);
	if (!ret) {
		writel(addr0, b->fmc + FMC_ADDR0);
		writel(addr1, b->fmc + FMC_ADDR1);
		writel((8 + 7) & 7, b->fmc + FMC_ADDRNUM);
		writel(0xb, b->fmc + FMC_RW_CTRL);
		ret = h2fmi_wait(fmi, b->fmc, FMC_STATUS, 0xb, 0xb);
	}
	if (!ret)
		ret = h2fmi_sdr_status(fmi, b, status);
	/* The package has committed to the data unless it reports a general error. */
	if (!ret && !(*status & BIT(4))) {
		ret = h2fmi_cmd1(fmi, b, 0x7a);
		if (!ret)
			ret = h2fmi_sdr_receive(fmi, b, buffer);
		end_ret = h2fmi_cmd1(fmi, b, 0x77);
	}
	writel(0, b->fmc + FMC_CE_CTRL);
	h2fmi_sdr_reset(b);
	return ret ? ret : end_ret;
}

/* Caller holds b->lock. All exits release CE and restore inherited state. */
static int h2fmi_read_transaction(struct h2fmi *fmi, struct h2fmi_bus *b,
				  u8 op, u8 op2, u32 address, int naddr,
				  u8 *buffer, u32 length, u8 *status)
{
	u32 on, if_ctrl;
	int ret, end_ret;

	*status = 0;
	if (b->failed)
		return -EIO;
	if (b->sdr) {
		ret = h2fmi_sdr_read(fmi, b, op, address, buffer, length, status);
		if (ret)
			h2fmi_fail(fmi);
		return ret;
	}
	on = readl(b->fmc + FMC_ON);
	if_ctrl = readl(b->fmc + FMC_IF_CTRL);
	if (on != 5)
		return -EIO;

	writel(1u << H2FMI_CE, b->fmc + FMC_CE_CTRL);
	h2fmi_clear_irqs(b);
	ret = h2fmi_cmd_addr_cmd(fmi, b, op, op2, address, naddr);
	if (!ret)
		ret = h2fmi_status(fmi, b, status);
	/* An error status has no data phase. Device parameters require GOOD;
	 * normal pages also admit REFRESH and ERASED. Keep the status for the
	 * MTD reader and bad-block query without copying a stale page buffer.
	 */
	if (!ret && (op == 0x92 ? *status == PPN_ST_GOOD :
		     h2fmi_page_status(*status) >= 0))
		ret = h2fmi_data_out(fmi, b, buffer, length);

	/* Stop PIO before ending the operation, as in the parameter reader. */
	writel(6, b->fmi + FMI_CONTROL);
	end_ret = h2fmi_cmd1(fmi, b, 0x77);
	writel(0, b->fmc + FMC_CE_CTRL);
	h2fmi_device_reset(b, on, if_ctrl);
	if (ret || end_ret) {
		h2fmi_fail(fmi);
		return ret ? ret : end_ret;
	}
	return 0;
}

static int h2fmi_read_geometry(struct h2fmi *fmi, struct h2fmi_bus *b)
{
	u8 status = 0;
	int ret;

	mutex_lock(&b->lock);
	ret = h2fmi_read_transaction(fmi, b, 0x92, 0x97, 0, 1,
				     b->page, H2FMI_PARAMS_SIZE, &status);
	if (!ret && status != PPN_ST_GOOD)
		ret = -EIO;
	if (!ret)
		ret = h2fmi_parse_geometry(b->page, H2FMI_PARAMS_SIZE,
					   &b->geometry);
	mutex_unlock(&b->lock);
	return ret;
}

/* Strip the FTL's metadata out of a raw page: 4 x (1024 data, 16 meta, 3072
 * data) becomes 16384 of data and, if asked, 64 of OOB. */
static void h2fmi_split_page(const u8 *raw, u8 *data, u8 *oob)
{
	int c;

	for (c = 0; c < H2FMI_CHUNKS; c++) {
		const u8 *src = raw + c * H2FMI_CHUNK_RAW;

		if (data) {
			u8 *dst = data + c * (H2FMI_CHUNK_LO + H2FMI_CHUNK_HI);

			memcpy(dst, src, H2FMI_CHUNK_LO);
			memcpy(dst + H2FMI_CHUNK_LO,
			       src + H2FMI_CHUNK_LO + H2FMI_CHUNK_META,
			       H2FMI_CHUNK_HI);
		}
		if (oob)
			memcpy(oob + c * H2FMI_CHUNK_META,
			       src + H2FMI_CHUNK_LO, H2FMI_CHUNK_META);
	}
}

static int h2fmi_ppn_read(void *priv, unsigned int bus, unsigned int cau,
			unsigned int block, unsigned int page, bool context,
			u8 *data, u8 *oob, u8 *status)
{
	struct h2fmi *fmi = priv;
	struct h2fmi_bus *b;
	u32 row;
	int ret;

	if (bus >= H2FMI_BUSES || !data || !oob || !status)
		return -EINVAL;
	b = &fmi->bus[bus];
	if (context)
		ret = h2fmi_slc_row(&b->geometry, cau, block, page, &row);
	else
		ret = h2fmi_page_row(&b->geometry, cau, block, page, &row);
	if (ret)
		return ret;

	/* Retired pages have no data phase. Never return a stale page buffer. */
	memset(data, 0, H2FMI_PAGE_DATA);
	memset(oob, 0, H2FMI_PAGE_OOB);
	ret = h2fmi_lock(fmi, b);
	if (ret)
		return ret;
	ret = h2fmi_read_transaction(fmi, b, 0x0a, 0x37, row, 4,
				     b->page, H2FMI_PAGE_RAW, status);
	if (!ret && h2fmi_page_status(*status) >= 0) {
		if (*status == PPN_ST_ERASED && memchr_inv(b->page, 0xff, H2FMI_PAGE_RAW))
			ret = -EIO;
		else
			h2fmi_split_page(b->page, data, oob);
	} else if (!ret && *status != PPN_ST_RETIRED) {
		ret = -EIO;
	}
	mutex_unlock(&b->lock);
	return ret;
}


/* ---- end of the Linux driver's part ---- */

/*
 * m1n1's main storage on these chips: the logical disk Apple's translation
 * layers keep on the two PPN packages, in the 4 KiB blocks the rest of m1n1
 * reads. iBoot leaves the controller clocked; nothing here switches clocks.
 */

static struct h2fmi flash;
static struct apple_ppn flash_ppn;
static struct apple_sftl *flash_sftl;
static u32 flash_pages;

static const struct apple_ppn_ops flash_ops = {
	.read_page = h2fmi_ppn_read,
};

static int flash_controller_init(void)
{
	int path[8];
	int i, ret;

	if (adt_path_offset_trace(adt, "/arm-io/flash-controller0", path) < 0) {
		printf("Storage: no /arm-io/flash-controller0\n");
		return -ENODATA;
	}

	/* Six regions, in the ADT's order: FMI, FMC, ECC for bus 0, then bus 1. */
	for (i = 0; i < H2FMI_BUSES; i++) {
		struct h2fmi_bus *b = &flash.bus[i];
		u64 fmi_reg, fmc_reg, ecc_reg;

		if (adt_get_reg(adt, path, "reg", i * 3 + 0, &fmi_reg, NULL) ||
		    adt_get_reg(adt, path, "reg", i * 3 + 1, &fmc_reg, NULL) ||
		    adt_get_reg(adt, path, "reg", i * 3 + 2, &ecc_reg, NULL)) {
			printf("Storage: flash controller bus %d has no registers\n", i);
			return -ENODATA;
		}
		b->fmi = (void *)(u32)fmi_reg;
		b->fmc = (void *)(u32)fmc_reg;
		b->ecc = (void *)(u32)ecc_reg;
		b->sdr = false;
		b->failed = false;
		if (!b->page)
			b->page = malloc(H2FMI_PAGE_RAW);
		if (!b->page)
			return -ENOMEM;

		printf("Storage: bus %d: FMC_ON=0x%08x IF_CTRL=0x%08x\n", i, readl(b->fmc + FMC_ON),
		       readl(b->fmc + FMC_IF_CTRL));
	}
	flash.reset_needed = false;
	flash.resets = 0;

	/* Without iBoot's DDR state, bring both packages up in SDR first. */
	if (readl(flash.bus[0].fmc + FMC_ON) != FMC_ON_DDR ||
	    readl(flash.bus[1].fmc + FMC_ON) != FMC_ON_DDR) {
		ret = h2fmi_sdr_bringup(&flash, NULL);
		if (ret) {
			printf("Storage: PPN SDR bring-up failed (%d)\n", ret);
			return ret;
		}
	}

	for (i = 0; i < H2FMI_BUSES; i++) {
		ret = h2fmi_read_geometry(&flash, &flash.bus[i]);
		if (ret) {
			printf("Storage: bus %d: PPN discovery failed (%d)\n", i, ret);
			return ret;
		}
		if (i && memcmp(&flash.bus[0].geometry, &flash.bus[i].geometry,
				sizeof(flash.bus[0].geometry))) {
			printf("Storage: PPN package geometries disagree\n");
			return -EINVAL;
		}
	}
	return 0;
}

bool main_storage_init(void)
{
	struct apple_sftl_info info;
	u64 start = get_ticks();
	int ret;

	if (flash_sftl)
		return true;

	ret = flash_controller_init();
	if (ret)
		return false;

	flash_ppn.ops = &flash_ops;
	flash_ppn.priv = &flash;
	flash_ppn.buses = H2FMI_BUSES;
	flash_ppn.caus = flash.bus[0].geometry.caus;
	flash_ppn.blocks = flash.bus[0].geometry.blocks;
	flash_ppn.pages = flash.bus[0].geometry.pages;

	printf("Storage: %u CAUs per bus, %u blocks of %u pages; reading the flash maps...\n",
	       flash_ppn.caus, flash_ppn.blocks, flash_ppn.pages);

	ret = apple_sftl_load(&flash_ppn, &flash_sftl);
	if (ret) {
		printf("Storage: the flash translation maps did not load (%d)\n", ret);
		flash_sftl = NULL;
		return false;
	}

	apple_sftl_get_info(flash_sftl, &info);
	flash_pages = info.disk_pages;
	printf("Storage: %u blocks of 4 KiB, %u map runs, ready after %lu ms\n", flash_pages,
	       info.nr_runs, ticks_to_msecs(get_ticks() - start));
	return true;
}

bool main_storage_read(u64 lba, u64 *bfr)
{
	int ret;

	if (!flash_sftl || lba >= flash_pages)
		return false;

	ret = apple_sftl_read(flash_sftl, lba, (u8 *)bfr);
	if (ret)
		printf("Storage: reading block %lu failed (%d)\n", lba, ret);
	return !ret;
}

bool main_storage_read_blocks(u64 lba, u32 count, void *bfr)
{
	u8 *out = bfr;

	for (; count; count--, lba++, out += SZ_4K)
		if (!main_storage_read(lba, (u64 *)out))
			return false;
	return true;
}

void main_storage_shutdown(void)
{
	apple_sftl_free(flash_sftl);
	flash_sftl = NULL;
}

bool main_storage_flush(void)
{
	/* read-only: nothing to flush */
	return true;
}
