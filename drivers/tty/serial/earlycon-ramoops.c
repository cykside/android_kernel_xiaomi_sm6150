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
 * The zone size defaults to 256K, the most a single early_ioremap() can map.
 * Give it the pstore *pmsg* prz, at the same address and the same size, with
 * console-size 0 and ecc-size 0.  Those two DT choices are load-bearing and
 * were learned the expensive way:
 *
 *   - prz->buffer_size is zone - 12, exactly this driver's ring, so the header
 *     is never read as "start > size"; with ecc-size non-zero the two owners of
 *     one header disagree on capacity and a reader erases what it calls
 *     invalid (capture 11, 2026-11-27 20:16).
 *   - any console-size at all makes pstore_register() add a console of its own,
 *     which untethers this earlycon ~0.2 s in and then writes its own records --
 *     DEFLATE-compressed here -- into the ring being read.  capture 14
 *     (2026-11-28) came back as 14225 bytes at 38 % printable, no DEFLATE
 *     stream start anywhere, because that is what the reading boot had been
 *     writing over the boot under test.
 *
 * pmsg is the right zone because /dev/pmsg0 from userspace is its only producer:
 * the kernel never writes a prz of its own accord here.  See
 * Documentation/arm64/surya-bringup.rst.
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
	 * This device has no separate recovery kernel: the TWRP environment that
	 * copies the capture out runs the very same Image and the same cmdline,
	 * so an earlycon that reset the header on every boot erased the log of
	 * the boot under test at 0.0 s of the boot that was about to read it.
	 * Every capture in this bring-up is therefore the *reader's* log -- still
	 * useful (same kernel, same DT, same defer chain, and a working UFS would
	 * show up as a scsi disk in it), but blind to a boot that dies before the
	 * reboot.  Appending instead of resetting makes the failing boot visible
	 * at the cost of a ring shared by a few boots, and the banner below is
	 * what splits them again.
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
