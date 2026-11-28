#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Fixture test for surya-ramoops-check.py: does the gate still reject the
# geometries that were each paid for with a flash round?
#
# The reason this file exists is that a gate over a device's RAM layout is
# worth nothing once nobody can say what it rejects.  Every expectation below
# is a mistake the bring-up actually made -- a console-size of 0, a reg that was
# 0x240000 instead of 0x400000, a sibling reservation over the reader's pmsg
# prz, an earlycon aimed at the wrong prz -- and each is built here as a real DTB
# with dtc, from the same text the passing fixture uses.
#
# A fixture that does not change the node is worse than no fixture: it looks
# like a pass.  So every mutant is byte-compared against the mirror's DTB and
# the test fails if the sed matched nothing.  (That is exactly how three
# "mutants" once came out identical to the base, a gate apparently approving
# them, and how an earlier version of this check spent a whole round.)
set -eu

work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
script_dir=$(dirname "$0")
check="$script_dir/surya-ramoops-check.py"
python=${PYTHON:-python3}
# dtc builds the fixtures and fdtget is what the gate reads properties with.  A
# missing tool must stop the test, not skip it: a suite that reports "0 failed"
# because nothing ran is the failure mode this whole file exists to avoid.
for tool in dtc fdtget; do
	command -v "$tool" >/dev/null 2>&1 || {
		echo "FATAL: $tool is not installed, so no fixture can be built or read" >&2
		exit 1
	}
done
pass=0
fail=0

# The node as the reader has it: the stock 4.14 recovery says `ramoops: attached
# 0x400000@0x9d800000, ecc: 0/0` in every capture, and with the vendor
# partitioning (record 0x80000, console 0x100000, pmsg 0x200000) that lays out
# dmesg x2, console at 0x9d900000 and pmsg at 0x9da00000.  ecc-size 0, not the
# vendor node's 0x10, for the reason in surya-sm6150.dts's comment.
write_mirror() {
	cat > "$work/mirror.dts" <<'DTS'
/dts-v1/;
/ {
	#address-cells = <2>;
	#size-cells = <2>;
	reserved-memory {
		#address-cells = <2>;
		#size-cells = <2>;
		ranges;

		ramoops@9d800000 {
			compatible = "ramoops";
			reg = <0x0 0x9d800000 0x0 0x00400000>;
			record-size = <0x80000>;
			pmsg-size = <0x200000>;
			console-size = <0x100000>;
			ecc-size = <0x0>;
			max-reason = <4>;
		};
	};
};
DTS
	dtc -I dts -O dtb -o "$work/mirror.dtb" "$work/mirror.dts" 2>"$work/dtc.log"
}

# mutant <name> <sed-expression>: one property changed inside the node, and the
# result required to differ from the mirror.
mutant() {
	name=$1
	expr=$2
	cp "$work/mirror.dts" "$work/$name.dts"
	sed -i "$expr" "$work/$name.dts"
	# cmp -s is true when the files are IDENTICAL: a mutant that changed no
	# text would compile to the mirror's own DTB and "reject" nothing.
	if cmp -s "$work/$name.dts" "$work/mirror.dts"; then
		echo "FATAL: fixture $name changed no text at all -- the expression matched nothing" >&2
		exit 1
	fi
	dtc -I dts -O dtb -o "$work/$name.dtb" "$work/$name.dts" 2>>"$work/dtc.log" || {
		echo "FATAL: fixture $name did not compile, so it could not reject anything" >&2
		cat "$work/dtc.log" >&2
		exit 1
	}
	if cmp -s "$work/$name.dtb" "$work/mirror.dtb"; then
		echo "FATAL: fixture $name produced a byte-identical DTB: it tests nothing" >&2
		exit 1
	fi
}

# want <dtb-name> <ok|reject> [needle] [-- extra check args...]
want() {
	name=$1
	expect=$2
	needle=${3:-}
	extra=${4:-}
	set +e
	if [ -n "$extra" ]; then
		out=$("$python" "$check" "$work/$name.dtb" $extra 2>&1)
	else
		out=$("$python" "$check" "$work/$name.dtb" 2>&1)
	fi
	rc=$?
	set -e
	if ! fdtget -t x "$work/$name.dtb" /reserved-memory/ramoops@9d800000 reg >/dev/null 2>&1 \
		&& [ "$name" != absent ]; then
		echo "FATAL: fixture $name has no ramoops node to check" >&2
		exit 1
	fi
	if [ "$expect" = ok ]; then
		want_rc=0
	else
		want_rc=1
	fi
	if [ "$rc" -ne "$want_rc" ]; then
		printf 'FAIL: %-9s exit %s, expected %s\n' "$name" "$rc" "$want_rc"
		printf '       %s\n' "$out"
		fail=$((fail + 1))
		return
	fi
	if [ -n "$needle" ] && ! printf '%s\n' "$out" | grep -q "$needle"; then
		printf 'FAIL: %-9s rejected, but not for the reason on record\n' "$name"
		printf '       expected: %s\n       got:      %s\n' "$needle" "$out"
		fail=$((fail + 1))
		return
	fi
	printf 'ok:   %-9s %s\n' "$name" "$expect"
	pass=$((pass + 1))
}

write_mirror
# The mirror itself has to pass, and say what it found -- including the no-map
# state, which the gate reports rather than requires.
want mirror ok "mirrors the reader"
printf '%s\n' "$("$python" "$check" "$work/mirror.dtb" 2>&1)" | grep -q "no-map absent" || {
	echo "FAIL: the mirror run does not report no-map at all" >&2
	exit 1
}

# no-map in the node is accepted and announced.  Deliberate: the flashable DTB
# has had it added by surya-sm6150-v2-test.dts:38 in every round so far, the
# four readable ones included, so no-map is not the discriminator between a
# readable capture and a torn one -- and the day someone decides it is, this
# fixture is where that claim gets tested instead of asserted.
mutant nomap '/compatible = "ramoops";/a\
\t\t\tno-map;'
want nomap ok "no-map present"

# Rounds 12 and 14 set console-size 0 to keep the log zone "unowned".  Both came
# back with /sys/fs/pstore mounted and not one file in it, with nothing in the
# reader's log to complain about: fs/pstore/ram.c returns before registering a
# pstore console, so nothing copies the printk buffer to RAM at all.
mutant console0 '/console-size/s/<0x100000>/<0x0>/'
# Both halves of the needle are required, because they are the two separate
# damages one console-size 0 does: our kernel loses its console prz writer
# (fs/pstore/ram.c:570 returns early) *and* the freed space is folded into the
# dmesg zones, sliding our deflate records onto the reader's console window --
# which is the file console-ramoops-14.txt actually was.
want console0 reject "returns before"
"$python" "$check" "$work/console0.dtb" 2>&1 | grep -q "dmesg prz set is 4" || {
	echo "FATAL: console0 must also be rejected for moving the dmesg przs" >&2
	exit 1
}
pass=$((pass + 1))

# (Both reg mutants spell the whole `s#old#new#` rather than `addr;s//new/`,
# because the reused RE is the address pattern, which spans `reg = <...>`, and
# the replacement then silently drops `reg = <` -- dtc's complaint was the only
# sign, and a fixture that does not compile rejects nothing.)
# An earlier round made the same window 0x40000 -- one earlycon map's worth --
# so our writes and the reader's export describe different przs.
mutant console40 '/console-size/s/<0x100000>/<0x40000>/'
want console40 reject "we would write one window"

# Capture 14 is what a shrunk reg does: 0x240000 left room for one dmesg prz
# plus the console window, so our second dmesg prz -- a deflate record -- landed
# on the reader's console prz, and the file came back 14225 B at 39.2 % printable.
mutant reg240 's#reg = <0x0 0x9d800000 0x0 0x00400000>#reg = <0x0 0x9d800000 0x0 0x00240000>#'
want reg240 reject "while the reader owns"

# And the opposite mistake, a reg twice as large: ramoops_probe() folds the
# leftover into the dmesg zones, so ten przs are laid out where the reader
# reserves two.
mutant reghuge 's#reg = <0x0 0x9d800000 0x0 0x00400000>#reg = <0x0 0x9d800000 0x0 0x00800000>#'
want reghuge reject "dmesg prz set is"

# The earlycon ring lives in the pmsg prz, so its size is the boundary of every
# prz after it; rounds 12 and 14 changed it and moved the console out from under
# the reader.
mutant pmsg0 '/pmsg-size/s/<0x200000>/<0x0>/'
want pmsg0 reject "earlycon ring at"
mutant pmsg40 '/pmsg-size/s/<0x200000>/<0x40000>/'
want pmsg40 reject "moves every prz boundary"

# record-size is the stride the reader walks crash records with.
mutant record40 '/record-size/s/<0x80000>/<0x40000>/'
want record40 reject "wrong stride"

# With the vendor node's ecc-size 0x10 our own probe calls the parity-less ring
# invalid -- three to four times at 0.085 s in console-ramoops-9.txt, -10.txt and
# -13.txt -- and resets its header, erasing the log of the boot before it.
mutant ecc16 '/ecc-size/s/<0x0>/<0x10>/'
want ecc16 reject "ecc-size 16"

# ftrace-size is not in the reader's map at all, and inserting a window shifts
# everything after it.
mutant ftrace '/max-reason = <4>;/i\
\t\t\tftrace-size = <0x1000>;'
want ftrace reject "ftrace-size"

# Round 14 also kept a second node, ramoops-log@9da00000, "owning" the log zone.
# dtc never complains about two reservations over the same bytes, and
# ramoops_probe() folds the leftover of reg into its dmesg przs, so the sibling
# silently changes where every boundary lands.
cp "$work/mirror.dts" "$work/sibling.dts"
sed -i 's#^\t};#\
\t\tramoops-log@9da00000 {\
\t\t\tcompatible = "ramoops";\
\t\t\treg = <0x0 0x9da00000 0x0 0x40000>;\
\t\t\tconsole-size = <0x0>;\
\t\t};\
\t};#' "$work/sibling.dts"
if cmp -s "$work/sibling.dts" "$work/mirror.dts"; then
	echo "FATAL: fixture sibling changed no text at all" >&2
	exit 1
fi
dtc -I dts -O dtb -o "$work/sibling.dtb" "$work/sibling.dts" 2>>"$work/dtc.log"
fdtget -t x "$work/sibling.dtb" /reserved-memory/ramoops-log@9da00000 reg >/dev/null || {
	echo "FATAL: fixture sibling has no ramoops-log node, so it tests nothing" >&2
	exit 1
}
want sibling reject "overlaps the reader's region"

# The earlycon address comes from the installer, not the DTB, so it is the one
# rule a fixture can only drive through --boot-test.  Aiming the ring at the
# reader's console prz instead of its pmsg prz is what the readable captures
# would have looked like had our own pstore console not also been writing there:
# two writers, one ring, and the header each of them validates differently.
printf 'earlycon=ramoops,0x9d900000,0x40000\n' > "$work/fake-installer.sh"
want mirror reject "but our pmsg prz starts at" "--boot-test $work/fake-installer.sh"

# A DTB whose node is gone must fail rather than be praised for having no sizes
# at all -- the shape a silent /delete-node/ in an audit layer leaves behind.
sed -e '/ramoops@9d800000 {/,/^\t\t};/d' "$work/mirror.dts" > "$work/absent.dts"
dtc -I dts -O dtb -o "$work/absent.dtb" "$work/absent.dts" 2>>"$work/dtc.log"
if fdtget -t x "$work/absent.dtb" /reserved-memory/ramoops@9d800000 reg >/dev/null 2>&1; then
	echo "FATAL: fixture absent still has the node; nothing was removed" >&2
	exit 1
fi
want absent reject "no usable reg property"

printf '\n%s passed, %s failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
