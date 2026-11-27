// SPDX-License-Identifier: GPL-2.0-only
/*
 * Boot console that logs into a ramoops zone.
 *
 * Meant for bring-up on devices without a UART: from the first printk on,
 * the kernel log is written into a ramoops zone using the persistent_ram
 * buffer layout, so after a warm reset it shows up in /sys/fs/pstore on
 * the next boot, even when that boot runs a different kernel.
 *
 *   earlycon=ramoops,<phys addr of the zone>[,<window size>[,<ECC zone size>]]
 *
 * The window size defaults to 256K, the most a single early_ioremap() can
 * map. Use a zone that nothing else writes to early, e.g. the pmsg zone.
 * Specify the full zone size only when its layout uses 16 ECC bytes
 * per 128-byte block, as configured on Surya.
 */

#include <linux/console.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/serial_core.h>
#include <linux/sizes.h>
#include <linux/string.h>

#include <asm/early_ioremap.h>

#define PERSISTENT_RAM_SIG	0x43474244	/* DBGC, see fs/pstore/ram_core.c */

struct ramoops_buffer {
	u32 sig;
	u32 start;
	u32 size;
	u8 data[];
};

/*
 * pstore probes in any later kernel (including the recovery one that
 * collects the log) zap the prz header: start and size go back to 0 while
 * the data survives.  Mirror the ring state at the end of the window,
 * where no known zap or record writer reaches, so the dump can be
 * rebuilt from the anchor even after the header was reset.
 */
#define EARLYCON_ANCHOR_SIG	0x45434c32	/* "2LCE" little-endian */
#define EARLYCON_ANCHOR_RESERVE	128

struct earlycon_anchor {
	u32 sig;
	u32 start;
	u32 size;
	u32 generation;
	u32 csum;
	/* zero pad up to EARLYCON_ANCHOR_RESERVE */
};

static struct ramoops_buffer __iomem *rbuf;
static struct earlycon_anchor __iomem *ranchor;
static phys_addr_t rbuf_phys;
static size_t rbuf_map_size;
static u32 rbuf_size;
static u32 ranchor_gen;
static void __iomem *ecc_buf;
static phys_addr_t ecc_phys;
static size_t ecc_map_size;

#define RAMOOPS_ECC_BYTES 16
#define RAMOOPS_ECC_BLOCK 128

/* Ramoops RS(255,239), first root 0, primitive polynomial 0x11d. */
static u8 earlycon_gf_mul(u8 a, u8 b)
{
	u8 result = 0;

	while (b) {
		if (b & 1)
			result ^= a;
		a = (a << 1) ^ ((a & 0x80) ? 0x1d : 0);
		b >>= 1;
	}
	return result;
}

static void earlycon_ecc_write(const void __iomem *src, size_t len,
			       void __iomem *dst)
{
	static const u8 gen[] = { 59, 13, 104, 189, 68, 209, 30, 8,
				  163, 65, 41, 229, 98, 50, 36, 59 };
	u8 parity[RAMOOPS_ECC_BYTES] = {};
	unsigned int i;

	while (len--) {
		u8 feedback = readb_relaxed(src++) ^ parity[0];

		for (i = 0; i < ARRAY_SIZE(gen) - 1; i++)
			parity[i] = parity[i + 1] ^ earlycon_gf_mul(feedback, gen[i]);
		parity[i] = earlycon_gf_mul(feedback, gen[i]);
	}
	for (i = 0; i < ARRAY_SIZE(parity); i++)
		writeb_relaxed(parity[i], dst + i);
}

static void earlycon_ecc_block(u32 index)
{
	earlycon_ecc_write(&rbuf->data[index * RAMOOPS_ECC_BLOCK],
			   RAMOOPS_ECC_BLOCK, ecc_buf + index * RAMOOPS_ECC_BYTES);
}

static void earlycon_ecc_header(void)
{
	earlycon_ecc_write(rbuf, sizeof(*rbuf),
			   ecc_buf + ecc_map_size - RAMOOPS_ECC_BYTES);
}

static void earlycon_anchor_update(void)
{
	struct earlycon_anchor a;

	a.sig = cpu_to_le32(EARLYCON_ANCHOR_SIG);
	a.start = cpu_to_le32(readl_relaxed(&rbuf->start));
	a.size = cpu_to_le32(readl_relaxed(&rbuf->size));
	a.generation = cpu_to_le32(ranchor_gen);
	a.csum = cpu_to_le32(EARLYCON_ANCHOR_SIG ^ ranchor_gen ^
			     le32_to_cpu(a.start) ^ le32_to_cpu(a.size));
	writel_relaxed(a.sig, &ranchor->sig);
	writel_relaxed(a.start, &ranchor->start);
	writel_relaxed(a.size, &ranchor->size);
	writel_relaxed(a.csum, &ranchor->csum);
	writel_relaxed(a.generation, &ranchor->generation);
	mb();
}


static void earlycon_ramoops_write(struct console *con, const char *s,
				   unsigned int n)
{
	u32 start, size;

	if (!rbuf)
		return;

	start = readl_relaxed(&rbuf->start);
	size = readl_relaxed(&rbuf->size);
	if (start >= rbuf_size || size > rbuf_size)
		return;
	while (n--) {
		writeb_relaxed(*s++, &rbuf->data[start]);
		if (ecc_buf && (!n || (start + 1) % RAMOOPS_ECC_BLOCK == 0))
			earlycon_ecc_block(start / RAMOOPS_ECC_BLOCK);
		if (++start == rbuf_size)
			start = 0;
		if (size < rbuf_size)
			size++;
	}
	writel_relaxed(start, &rbuf->start);
	writel_relaxed(size, &rbuf->size);
	if (ecc_buf)
		earlycon_ecc_header();
	if (ranchor) {
		ranchor_gen++;
		earlycon_anchor_update();
	}
	/* make sure it reached DDR before a reset can hit */
	mb();
}

static int __init earlycon_ramoops_setup(struct earlycon_device *dev,
					 const char *opt)
{
	unsigned long long size = SZ_256K, zone = 0;
	char options[sizeof(dev->options)], *zone_opt;
	size_t blocks, offset;

	if (!dev->port.mapbase)
		return -ENODEV;
	/* register_earlycon() consumes the address and saves its suffix. */
	if (!opt)
		opt = dev->options;
	if (strscpy(options, opt, sizeof(options)) < 0)
		return -EINVAL;
	zone_opt = strchr(options, ',');
	if (zone_opt) {
		*zone_opt++ = '\0';
		if (kstrtoull(zone_opt, 0, &zone) || !zone || zone > SZ_2M)
			return -EINVAL;
	}
	if (*options && kstrtoull(options, 0, &size))
		return -EINVAL;
	if (size <= sizeof(*rbuf) || size > SZ_256K)
		return -EINVAL;

	rbuf_phys = dev->port.mapbase;
	rbuf_map_size = size;
	if (size <= sizeof(*rbuf) + EARLYCON_ANCHOR_RESERVE)
		return -EINVAL;
	rbuf_size = round_down(size - sizeof(*rbuf) - EARLYCON_ANCHOR_RESERVE,
			       RAMOOPS_ECC_BLOCK);
	if (zone) {
		if (zone <= size + RAMOOPS_ECC_BYTES ||
		    !IS_ALIGNED(rbuf_phys, PAGE_SIZE) || rbuf_phys + zone < rbuf_phys)
			return -EINVAL;
		/* Same parity placement as persistent_ram_init_ecc(). */
		blocks = DIV_ROUND_UP(zone - sizeof(*rbuf) - RAMOOPS_ECC_BYTES,
				      RAMOOPS_ECC_BLOCK + RAMOOPS_ECC_BYTES);
		ecc_map_size = (blocks + 1) * RAMOOPS_ECC_BYTES;
		offset = zone - ecc_map_size;
		if (!rbuf_size || PAGE_ALIGN(size) > round_down(offset, PAGE_SIZE) ||
		    offset - round_down(offset, PAGE_SIZE) + ecc_map_size > SZ_256K)
			return -EINVAL;
		ecc_phys = rbuf_phys + offset;
		ecc_buf = early_ioremap(ecc_phys, ecc_map_size);
		if (!ecc_buf)
			return -ENOMEM;
	}
	rbuf = early_ioremap(rbuf_phys, rbuf_map_size);
	if (!rbuf) {
		if (ecc_buf) {
			early_iounmap(ecc_buf, ecc_map_size);
			ecc_buf = NULL;
		}
		return -ENOMEM;
	}

	writel_relaxed(0, &rbuf->start);
	writel_relaxed(0, &rbuf->size);
	writel_relaxed(PERSISTENT_RAM_SIG, &rbuf->sig);
	if (ecc_buf)
		earlycon_ecc_header();
	/* Publish the anchor even with an empty ring: its magic proves
	 * that earlycon came up, which tells a dead kernel from a dead
	 * log when the collector finds nothing in pstore.
	 */
	ranchor = (struct earlycon_anchor __iomem *)
		((u8 __iomem *)rbuf + rbuf_map_size - EARLYCON_ANCHOR_RESERVE);
	ranchor_gen = 0;
	earlycon_anchor_update();
	mb();

	dev->con->write = earlycon_ramoops_write;
	return 0;
}
EARLYCON_DECLARE(ramoops, earlycon_ramoops_setup);

/* Move off the early fixmap mapping once ioremap() works. */
static int __init earlycon_ramoops_remap(void)
{
	struct ramoops_buffer __iomem *old = rbuf, *new;
	void __iomem *old_ecc = ecc_buf, *new_ecc = NULL;

	if (!old)
		return 0;

	new = ioremap(rbuf_phys, rbuf_map_size);
	if (!new)
		return 0;

	if (old_ecc) {
		new_ecc = ioremap(ecc_phys, ecc_map_size);
		if (!new_ecc) {
			iounmap(new);
			return 0;
		}
	}

	console_lock();
	rbuf = new;
	ecc_buf = new_ecc;
	ranchor = (struct earlycon_anchor __iomem *)
		((u8 __iomem *)new + rbuf_map_size - EARLYCON_ANCHOR_RESERVE);
	earlycon_anchor_update();
	console_unlock();
	early_iounmap(old, rbuf_map_size);
	if (old_ecc)
		early_iounmap(old_ecc, ecc_map_size);
	return 0;
}
early_initcall(earlycon_ramoops_remap);
