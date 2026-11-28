#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Report DTBO compatibility without modifying inputs or saving a merged DTB.
set -eu

if [ "$#" -lt 2 ] || [ "$#" -gt 3 ]; then
	printf 'Usage: %s BASE.dtb OVERLAY.dtbo [REFERENCE.dtb]\n' "$0" >&2
	exit 2
fi
base=$1
overlay=$2
reference=${3:-}
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
export LC_ALL=C

# Validate FDT inputs before treating absent optional nodes as empty.
fdtget -l "$overlay" / > "$work/overlay-nodes"
fdtget -l "$base" / > "$work/base-nodes"
: > "$work/fixups"
: > "$work/symbols"
if grep -Fxq __fixups__ "$work/overlay-nodes"; then
	fdtget -p "$overlay" /__fixups__ > "$work/fixups"
fi
if grep -Fxq __symbols__ "$work/base-nodes"; then
	fdtget -p "$base" /__symbols__ > "$work/symbols"
fi
sort -u -o "$work/fixups" "$work/fixups"
sort -u -o "$work/symbols" "$work/symbols"
comm -23 "$work/fixups" "$work/symbols" > "$work/missing"
printf 'Missing external symbols: %s/%s\n' "$(wc -l < "$work/missing")" "$(wc -l < "$work/fixups")"
while IFS= read -r label; do
	if [ -n "$reference" ]; then
		target=$(fdtget -t s "$reference" /__symbols__ "$label" 2>/dev/null || printf '%s' '(not in reference)')
		printf '%s\t%s\n' "$label" "$target"
	else
		printf '%s\n' "$label"
	fi
done < "$work/missing"

if fdtoverlay -i "$base" -o "$work/merged.dtb" "$overlay"; then
	printf 'Overlay applied offline; hardware and bootloader behavior unverified.\n'
else
	printf 'Overlay rejected offline.\n' >&2
	exit 1
fi
