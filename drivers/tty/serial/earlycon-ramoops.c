// SPDX-License-Identifier: GPL-2.0-only
/*
 * Boot console that logs into a ramoops zone.
 *
 * Meant for bring-up on devices without a UART: from the first printk on,
 * the kernel log is written into a ramoops zone using the persistent_ram
 * buffer layout, so after a warm reset it shows up in /sys/fs/pstore on
 * the next boot, even when that boot runs a different kernel.
 *
 *   earlycon=ramoops,<phys addr of the zone>[,<zone size>]
 *
 * The zone size defaults to 256K, the most a single early_ioremap() can map;
 * the address has no default at all, so an image flashed without the installer's
 * Give the zone the recovery kernel's own geometry, and let it be a prz of
 * ours: on surya that is the pmsg prz at 0x9da00000 inside
 * /reserved-memory/ramoops@9d800000, which arch/arm64/boot/dts/vendor/qcom/
 * surya-sm6150.dts now lays out to match the recovery exactly.  That is a
 * change of mind, bought with a flash round, and the cost is stated below
 * rather than hidden:
 *
 *   - Any prz of ours is snapshotted and then RESET at our own probe:
 *     ramoops_init_prz() passes PRZ_FLAG_ZAP_OLD (fs/pstore/ram.c:589) and
 *     persistent_ram_post_init() falls through to persistent_ram_zap() even on
 *     the "header looks valid" path (fs/pstore/ram_core.c:527-568).  So on a
 *     boot that lives past pstore_register() (~0.2 s) the text this console
 *     wrote is erased from the ring.  It is not lost, because the pstore console
 *     then replays the whole printk buffer from 0.000000 s into its own prz --
 *     and that channel, the reader's console prz, is where every readable capture
 *     of this bring-up came from: console-ramoops-8.txt, -9.txt, -10.txt (whole
 *     boot, "Rebooting in 5 seconds.." last line) and -13.txt, 100 % printable
 *     from byte 0, none of them containing a single line of this earlycon
 *     (grep -c 'surya earlycon' says 0 in all four).
 *   - The value of this console is therefore exactly the boot that never
 *     reaches pstore_register(): hung before 0.2 s, panic inside an early init
 *     call, a watchdog that fires before userspace.  For that boot nothing else
 *     writes the prz, so the ring it fills survives untouched into the next
 *     boot, which is what the takeover logic below is for.
 *   - The capacity has to be what the reader computes for the same window.  It
 *     attaches with `ecc: 0/0`, so its prz->buffer_size is zone - 12 and this
 *     driver's rbuf_size is zone - 12 too; the DT keeps ecc-size 0 for the same
 *     reason, because with ecc-size 0x10 our own probe prints "uncorrectable
 *     error in header" once per prz (three to four times at 0.085 s in
 *     console-ramoops-9.txt, -10.txt and -13.txt) and resets the ring here.
 *
 * The kernel that reads this back is the stock 4.14 recovery, which the captures
 * themselves prove -- "pstore: using zlib compression" and "ramoops: attached
 * 0x400000@0x9d800000, ecc: 0/0" exist only in that tree; this one prints
 * "pstore: Using crash dump compression: %s" and has no "attached" line at all.
 * 4.14 also has no PRZ_FLAG_ZAP_OLD, so a valid header it finds is snapshotted
 * with persistent_ram_save_old() and left intact: the same text is still in the
 * window after it has been read, which is why re-running the collector is free.
 */

#include <linux/console.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/serial_core.h>
#include <linux/sizes.h>

#include <asm/early_ioremap.h>

#define PERSISTENT_RAM_SIG	0x43474244	/* DBGC, see fs/pstore/ram_core.c */

struct ramoops_buffer {
	u32 sig;
	u32 start;
	u32 size;
	u8 data[];
};

static struct ramoops_buffer __iomem *rbuf;
static phys_addr_t rbuf_phys;
static size_t rbuf_map_size;
static u32 rbuf_size;

static void earlycon_ramoops_write(struct console *con, const char *s,
				   unsigned int n)
{
	unsigned int i;
	u32 end, start, size;

	if (!rbuf || !n)
		return;

	start = readl_relaxed(&rbuf->start);
	size = readl_relaxed(&rbuf->size);
	if (n > rbuf_size)
		n = rbuf_size;

	/*
	 * Publish the header for this record *before* writing its payload, and
	 * write the two header words in the order readers validate against.
	 *
	 * A persistent_ram zone is only exported when start <= size <= capacity.
	 * This console exists for a board that dies inside a printk, on the CPU
	 * that holds the log lock, so a reset cuts the payload and the header
	 * apart, and the old order (payload, then start, then size) left the
	 * window where start had moved ahead of a size that never caught up --
	 * which is exactly what the recovery kept reporting while our text sat
	 * right there: 2026-11-27 20:16 "persistent_ram: found existing invalid
	 * buffer, size 57821, start 192977", and 23:56 "size 52101, start
	 * 52133", a 32-byte gap, one printk record.  Worse than losing a
	 * capture: 4.14's reader *zaps* a buffer it calls invalid, so the tear
	 * also destroyed the previous record.
	 *
	 * size first (it only ever grows, so start <= size holds at every
	 * instant), then start, then the bytes.  A crash now costs at most the
	 * tail of the last record; the zone stays exported.
	 */
	end = start;
	for (i = 0; i < n; i++) {
		if (++end == rbuf_size)
			end = 0;
		if (size < rbuf_size)
			size++;
	}
	writel_relaxed(size, &rbuf->size);
	writel_relaxed(end, &rbuf->start);
	/* order the header words ahead of the payload */
	mb();

	for (i = 0; i < n; i++) {
		writeb_relaxed(*s++, &rbuf->data[start]);
		if (++start == rbuf_size)
			start = 0;
	}
	mb();
}

static int __init earlycon_ramoops_setup(struct earlycon_device *dev,
					 const char *opt)
{
	unsigned long long size = SZ_256K;

	if (!dev->port.mapbase)
		return -ENODEV;
	if (opt && *opt && kstrtoull(opt, 0, &size))
		return -EINVAL;
	if (size <= sizeof(*rbuf) || size > SZ_256K)
		return -EINVAL;

	rbuf_phys = dev->port.mapbase;
	rbuf_map_size = size;
	rbuf = early_ioremap(rbuf_phys, rbuf_map_size);
	if (!rbuf)
		return -ENOMEM;
	rbuf_size = size - sizeof(*rbuf);

	/*
	 * Take over the ring a previous boot left, rather than wiping it.
	 *
	 * The header is the only thing that says whether this text is worth
	 * reading, and the reader needs it intact: the recovery kernel's ramoops
	 * driver snapshots the zone into its own old-buffer cache in
	 * persistent_ram_post_init() and only on the path where the header still
	 * looks valid -- "found existing empty buffer" returns before the
	 * snapshot, and the record is never exported at all.  So a boot that
	 * resets the header here deletes the log of the boot before it at 0.0 s
	 * of the boot that was about to read it.  Appending instead costs a ring
	 * shared by a few boots, and the banner below is what splits them again.
	 *
	 * The zap our own prz gets (PRZ_FLAG_ZAP_OLD, header comment above) is
	 * why a boot that survives to pstore_register() loses this ring: its
	 * header is reset at ~0.2 s and the pstore console's own prz is what
	 * survives.  A boot that dies before that never reaches the zap, and
	 * that is the boot this console exists for.
	 *
	 * Only a header this driver could have written is kept: anything else --
	 * a foreign signature, a size past this zone's capacity, start > size --
	 * is the "found existing invalid buffer" case that a pstore reader erases
	 * on sight, so it is reset here instead.
	 */
	if (readl_relaxed(&rbuf->sig) != PERSISTENT_RAM_SIG ||
	    readl_relaxed(&rbuf->size) > rbuf_size ||
	    readl_relaxed(&rbuf->start) > readl_relaxed(&rbuf->size)) {
		writel_relaxed(0, &rbuf->size);
		writel_relaxed(0, &rbuf->start);
		writel_relaxed(PERSISTENT_RAM_SIG, &rbuf->sig);
		mb();
	}

	dev->con->write = earlycon_ramoops_write;

	/*
	 * Name the boot at its start.  linux_banner is the only string that can
	 * distinguish two boots of a kernel that otherwise prints the same lines,
	 * and it is the check that a report belongs to the build it claims to:
	 * a stale tree in /tmp has already been nearly flashed twice.
	 */
	{
		char mark[256];
		int len = snprintf(mark, sizeof(mark), "\n==== surya earlycon, new boot: %s",
				   linux_banner);

		if (len > 0)
			earlycon_ramoops_write(dev->con, mark,
					       len < (int)sizeof(mark) ? len : (int)sizeof(mark) - 1);
	}
	return 0;
}
EARLYCON_DECLARE(ramoops, earlycon_ramoops_setup);

/* Move off the early fixmap mapping once ioremap() works. */
static int __init earlycon_ramoops_remap(void)
{
	struct ramoops_buffer __iomem *old = rbuf, *new;

	if (!old)
		return 0;

	new = ioremap(rbuf_phys, rbuf_map_size);
	if (!new)
		return 0;

	console_lock();
	rbuf = new;
	console_unlock();
	early_iounmap(old, rbuf_map_size);
	return 0;
}
early_initcall(earlycon_ramoops_remap);
