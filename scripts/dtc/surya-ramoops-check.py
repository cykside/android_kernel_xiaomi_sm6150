#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check a Surya DTB's pstore reservation against what the recovery kernel reads.

    usage: surya-ramoops-check.py <dtb> [<dtb> ...]

Exit status is 0 only if every DTB passed.  This exists because the log we
need is produced by two kernels: ours writes the RAM, the stock 4.14 recovery
kernel reads it, and the reader only ever exports persistent RAM zones at the
addresses *it* was told about.  Those addresses are not ours to choose, so the
node has to mirror them -- and the tree has three .dts files that can describe
this node (surya-sm6150.dts, plus the two offline-audit layers that the
flashable DTB is actually built from), so "one file is right" proves nothing.
Pass every DTB that ships or boots; each is checked against the same map.

The numbers below are quoted from device evidence, not invented:

  twrp-pstore-facts-2.txt .. -11.txt -- every round, four identical lines, the
  recovery kernel describing its own attachment:

    console [pstore0] enabled
    pstore: Registered ramoops as persistent store backend
    ramoops: attached 0x400000@0x9d800000, ecc: 0/0

  arch/arm64/boot/dts/vendor/qcom/sdmmagpie.dtsi:669 of the 4.14 source -- the
  vendor node that produces that geometry: record 0x80000, console 0x100000,
  pmsg 0x200000.  Its ecc-size <0x10> is the one property NOT copied, for the
  reason the ecc rule below states with the capture lines that prove it.

  console-ramoops-8.txt, -9.txt, -10.txt, -13.txt -- the four readable
  captures, every one of them built from a node with this shape (git show
  <build>:.../surya-sm6150.dts says so for all four), and
  console-ramoops-14.txt, built from the round that set console-size 0.

Two claims this file used to make and no longer does, because the tree
contradicted them.  First, that the readable rounds lacked `no-map`: they did
not -- the flashable DTB has had it added by surya-sm6150-v2-test.dts:38 in
every round since 3fd371679, so no-map is not the discriminator between a
readable capture and a torn one and this script does not claim it is.  Second,
that the zaps in rounds 1 and 11 came from ECC: the sizes those rounds printed
(57821, 52101) are prz geometry disagreements caused by their own reg values,
and the ecc effect is a different one, described at the ecc rule.

Documentation/arm64/surya-bringup.rst carries the full table of rounds.
"""

import argparse
import os
import re
import subprocess
import sys

NODE = "/reserved-memory/ramoops@9d800000"

# What the reader declared for itself, as one dict so that every rule below is
# stated as "agree with the reader" rather than as a magic number.
READER = dict(base=0x9d800000, size=0x400000, record=0x80000,
              console=0x100000, ftrace=0, pmsg=0x200000, ecc=0)

# The earlycon that writes the ring before pstore exists.  Its address is not
# free either: it has to be the reader's pmsg prz, which is derived from the
# geometry above, so it is parsed out of the script that patches the cmdline.
EARLYCON_MAX = 0x40000  # one early_ioremap()

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def fail(lines, text):
    lines.append(text)


def fdtget(*args):
    """Run fdtget with the CLI's own argument order (flags, file, node, prop).

    Getting that order wrong is silent: fdtget happily treats a node name as the
    file argument and reports "not found", which a gate that only looks at
    return codes reads as "property absent" -- and then approves the build.
    """
    return subprocess.run(["fdtget"] + list(args), capture_output=True,
                          text=True, check=False)


def num(tok, what):
    """fdtget -t x prints unprefixed hex, some builds prefix it.  Parse both,
    and refuse anything else: a gate that reads a property as zero because the
    output format changed has approved nothing."""
    try:
        return int(tok[2:], 16) if tok[:2].lower() == "0x" else int(tok, 16)
    except ValueError:
        sys.exit("ramoops gate: %r for %s is not a hex number -- fdtget on %s "
                 "returned something unexpected, so nothing derived from it "
                 "means anything" % (tok, what, NODE))


def cells(dtb, name, errors, default=None):
    out = fdtget("-t", "x", dtb, NODE, name)
    if out.returncode:
        return default
    toks = out.stdout.split()
    if not toks:
        fail(errors, "%s: property %s is present but empty, so no size can be "
                    "read from it" % (NODE, name))
        return default
    return [num(t, name) for t in toks]


def layout(start, total, console, ftrace, pmsg, record):
    """ramoops_probe(): dump = size - console - ftrace - pmsg, one prz per
    record-size, zone = dump / cnt rounded down.  The leftover folds into the
    dmesg zones, so the layout must be computed and never assumed -- that is
    exactly how a 0x240000 reg parked our dmesg prz #2 on the reader's console
    window and handed back 14225 bytes of deflate instead of a log."""
    dump = total - console - ftrace - pmsg
    cnt = dump // record if record else 0
    zone = (dump // cnt) & ~1 if cnt else 0
    dmesg = [start + i * zone for i in range(cnt)]
    con = start + cnt * zone
    return dump, cnt, zone, dmesg, con, con + console, con + console + ftrace


def check(dtb, boot_test=None):
    """Return a list of reasons this DTB must not boot."""
    errors = []
    region = cells(dtb, "reg", errors)
    if region is None or len(region) < 2:
        return ["%s: fdtget found no usable reg property (%r) in %s -- the node "
                "or the dtb is missing, so every address derived below would be "
                "a guess" % (NODE, region, dtb)]
    # #address-cells/#size-cells are 2: "0 9d800000 0 400000", so the low halves
    # are the third and the last cell.
    if len(region) >= 4:
        base, size = region[-3], region[-1]
    else:
        base, size = region[-2], region[-1]
    zero = lambda name, default=0: (cells(dtb, name, errors) or [default])[-1]
    console, ftrace = zero("console-size"), zero("ftrace-size")
    pmsg, record = zero("pmsg-size"), zero("record-size", READER["record"])
    ecc = zero("ecc-size")

    if fdtget("-t", "s", dtb, NODE, "compatible").returncode:
        fail(errors, 'no compatible = "ramoops" on %s, so no driver binds it '
                     "and the whole reservation is dead RAM" % NODE)

    installer = boot_test or os.path.join(ROOT, "tools/surya-anykernel",
                                          "boot-test.sh")
    m = re.search(r"earlycon=ramoops,(0x[0-9a-f]+),(0x[0-9a-f]+)",
                  open(installer).read())
    if not m:
        fail(errors, "%s must name the ramoops earlycon zone" % installer)
        ec_addr = ec_size = 0
    else:
        ec_addr = num(m.group(1), "earlycon addr")
        ec_size = num(m.group(2), "earlycon size")

    o_dump, o_cnt, o_zone, o_dmesg, o_con, o_pmsg, o_end = layout(
        base, size, console, ftrace, pmsg, record)
    r_dump, r_cnt, r_zone, r_dmesg, r_con, r_pmsg, r_end = layout(
        READER["base"], READER["size"], READER["console"], READER["ftrace"],
        READER["pmsg"], READER["record"])

    if base != READER["base"] or size != READER["size"]:
        fail(errors, "reg 0x%x+0x%x while the reader owns 0x%x+0x%x: pstore "
                     "exports przs at the *reader's* addresses, so any shift "
                     "makes our text land in a prz it calls by another name"
                     % (base, size, READER["base"], READER["size"]))
    # console-size is the difference between a readable capture and a window
    # full of somebody else's bytes.  ramoops_init_prz() returns here when the
    # size is 0, so our kernel has no console prz to write -- and the reader
    # still exports 0x9d900000 as console-ramoops-0, now showing whatever our
    # dmesg przs landed on: console-ramoops-14.txt (14225 B, 39.2 % printable,
    # deflate records) and console-ramoops-12.txt (66728 B, 94.7 %, first line
    # torn) are both that, from the two rounds that set console-size 0.
    if console == 0:
        fail(errors, "console-size 0: fs/pstore/ram.c:570 returns before "
                     "creating the prz, so our kernel writes nothing at "
                     "0x9d900000 while the reader still exports that window as "
                     "console-ramoops-0 -- see console-ramoops-14.txt (39.2 % "
                     "printable) and -12.txt (torn head)")
    elif console != READER["console"]:
        fail(errors, "console-size 0x%x != the reader's 0x%x: we would write one "
                     "window and it would read another" % (console, READER["console"]))
    if pmsg != READER["pmsg"]:
        fail(errors, "pmsg-size 0x%x != the reader's 0x%x: the earlycon ring at "
                     "0x%x lives in that window, and a size that does not match "
                     "moves every prz boundary after it"
                     % (pmsg, READER["pmsg"], ec_addr))
    if record != READER["record"]:
        fail(errors, "record-size 0x%x != the reader's 0x%x, so a panic record "
                     "we leave behind is read back at the wrong stride"
                     % (record, READER["record"]))
    # ecc-size must stay 0, and the reason is in the readable captures
    # themselves, not in a theory.  The vendor node says <0x10>, so our four
    # readable rounds built with ecc on -- and 3 to 4 lines later in the same
    # capture, at 0.085 s, our own probe rejects the earlycon's parity-less
    # ring:
    #
    #   console-ramoops-13.txt:205  [    0.085229][    T1] ramoops:
    #                               uncorrectable error in header
    #   (the same line 3x in -9.txt, 4x in -10.txt, 4x in -13.txt, 0x in -8.txt)
    #
    # That is fs/pstore/ram_core.c:249, reached from persistent_ram_check_valid()
    # with prz->ecc_info.ecc_size set, and its caller then resets the header --
    # so an ecc-enabled prz erases the log of the boot before it, which is the
    # only thing the earlycon is for.  The reader agrees with 0 anyway: it
    # prints `ecc: %d/%d` = cxt->ecc_info.ecc_size/block_size (4.14 ram.c:878)
    # and every round shows `ecc: 0/0`.
    if ecc != READER["ecc"]:
        fail(errors, "ecc-size %d: prz->buffer_size becomes zone - 12 - "
                     "ecc_total with the parity tail at buffer->data + "
                     "buffer_size (ram_core.c:216-219), so our probe calls a "
                     "zone written without parity invalid and resets its header "
                     "-- deleting the previous boot's earlycon log at 0.085 s "
                     "(see the lines above), and disagreeing with a reader that "
                     "attaches with ecc 0/0" % ecc)
    if ftrace:
        fail(errors, "ftrace-size 0x%x: the reader declares none, so every "
                     "window after it shifts by that amount" % ftrace)
    if o_cnt != r_cnt or o_zone != r_zone:
        fail(errors, "dmesg prz set is %d x 0x%x where the reader expects %d x "
                     "0x%x: our records would land in its console or pmsg window"
                     % (o_cnt, o_zone, r_cnt, r_zone))
    if any(x not in r_dmesg for x in o_dmesg):
        fail(errors, "dmesg prz bases %s are not the reader's %s"
             % ([hex(x) for x in o_dmesg], [hex(x) for x in r_dmesg]))
    if o_con != r_con:
        fail(errors, "console prz at 0x%x but the reader reads its own at 0x%x "
                     "-- that window is the channel every readable capture in "
                     "this bring-up came out of" % (o_con, r_con))
    if o_end > READER["base"] + READER["size"]:
        fail(errors, "our przs end at 0x%x, past the region the reader reserved "
                     "(0x%x)" % (o_end, READER["base"] + READER["size"]))
    if ec_addr != o_pmsg:
        fail(errors, "earlycon at 0x%x but our pmsg prz starts at 0x%x: the ring "
                     "would sit outside the window we reserve, in RAM the page "
                     "allocator may use" % (ec_addr, o_pmsg))
    if ec_addr != r_pmsg:
        fail(errors, "earlycon at 0x%x but the reader's pmsg prz starts at 0x%x: "
                     "the text would be in no prz it exports" % (ec_addr, r_pmsg))
    if ec_size > pmsg:
        fail(errors, "earlycon zone 0x%x is larger than the prz that hosts it "
                     "(0x%x)" % (ec_size, pmsg))
    if ec_size > EARLYCON_MAX:
        fail(errors, "earlycon zone 0x%x exceeds one early_ioremap (%dK)"
             % (ec_size, EARLYCON_MAX // 1024))
    # The ring's capacity has to be a number the reader also accepts, or the
    # header we publish reads as invalid to it and a zap follows.  This driver
    # computes zone - 12 with no parity of its own, so the only safe direction is
    # "no bigger than the reader's own view of that prz", which is
    # min(zone - 12, zone - 12 - ecc_total) and is <= zone - 12 either way.
    cap_ours = ec_size - 12
    cap_theirs = READER["pmsg"] - 12
    if cap_ours > cap_theirs:
        fail(errors, "ring capacity 0x%x exceeds the reader's pmsg capacity "
                     "0x%x: a start we consider normal is out of range for it, "
                     "and 4.14 zaps what it calls invalid (round 11: 'found "
                     "existing invalid buffer, size 57821, start 192977')"
                     % (cap_ours, cap_theirs))

    # no-map is reported, not required.  The board node omits it because the
    # vendor node it mirrors does (sdmmagpie.dtsi:669), and the flashable DTB is
    # built from the offline layers, where surya-sm6150-v2-test.dts:38 has added
    # `no-map;` to this node since 3fd371679 -- so every capture so far, the four
    # readable ones included, came from a DTB that had it.  That makes no-map
    # unproven in both directions, and this gate exists to reject what is proven
    # wrong; a line here says which way a build went, so a round can be compared
    # with a round.
    no_map = fdtget(dtb, NODE, "no-map").returncode == 0
    # No sibling reservation.  ramoops_probe() folds whatever is left of `reg`
    # into its dmesg przs, so a second compatible="ramoops" node over the same
    # bytes is a double reservation that dtc never complains about and that
    # silently steals the window the reader exports.
    listing = fdtget("-l", dtb, "/reserved-memory").stdout.split()
    for sibling in listing:
        if not sibling.startswith("ramoops") and "ramoops" not in sibling:
            continue
        if sibling == "ramoops@9d800000":
            continue
        reg = fdtget("-t", "x", dtb, "/reserved-memory/" + sibling,
                     "reg").stdout.split()
        vals = [num(t, "sibling reg") for t in reg]
        # #address-cells/#size-cells are 2 in the Surya DTBs; accept the 1+1
        # shape too rather than reading a 1-cell reg as a 2-cell one.
        if len(vals) >= 4:
            sib_base, sib_size = vals[-3], vals[-1]
        elif len(vals) >= 2:
            sib_base, sib_size = vals[-2], vals[-1]
        else:
            sib_base = sib_size = None
        if sib_size is not None and \
                sib_base < READER["base"] + READER["size"] and \
                sib_base + sib_size > READER["base"]:
            fail(errors, "/reserved-memory/%s overlaps the reader's region: with "
                         "pmsg-size non-zero that window is a prz of ours "
                         "already, and a second node over the same bytes is a "
                         "double reservation -- ramoops_probe() folds the "
                         "leftover of reg into its dmesg przs, so it changes "
                         "where every prz boundary lands" % sibling)
    return errors


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("dtb", nargs="+", help="flattened device tree to check")
    # Only so scripts/dtc/test-surya-ramoops-check.sh can drive the earlycon half
    # of the check with a fixture instead of the real installer; nothing in CI
    # passes it, so a shipped build is always measured against the cmdline the
    # zip really patches.
    ap.add_argument("--boot-test", default=None,
                    help="installer script to read earlycon= from (default: "
                         "tools/surya-anykernel/boot-test.sh)")
    args = ap.parse_args(argv)
    bad = 0
    for dtb in args.dtb:
        errors = check(dtb, args.boot_test)
        no_map = fdtget(dtb, NODE, "no-map").returncode == 0
        if errors:
            bad += 1
            for e in errors:
                print("ramoops gate: %s: %s" % (dtb, e), file=sys.stderr)
            continue
        print("ramoops gate: %s mirrors the reader (0x%x+0x%x, ecc 0x%x, "
              "no-map %s): 2 x 0x80000 dmesg, console 0x9d900000 + 0x100000, "
              "pmsg 0x9da00000 + 0x200000, and the earlycon ring lands inside "
              "that pmsg prz"
              % (dtb, READER["base"], READER["size"], READER["ecc"],
                 "present" if no_map else "absent"))
    return 1 if bad else 0


if __name__ == "__main__":
    # sys.exit, not main(): a gate whose exit status is dropped approves every
    # build it has ever seen, and every fixture would say "correct".
    sys.exit(main())
