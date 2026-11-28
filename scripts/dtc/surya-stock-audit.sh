#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Merge the MIUI 14.0.1 Surya reference pair for offline inspection only.
set -eu

if [ "$#" -ne 3 ]; then
	echo "Usage: $0 BASE.dtb IDP.dtbo-or-dts OUTPUT_DIR" >&2
	exit 2
fi
base=$1
overlay=$2
out=$3
for tool in fdtget fdtoverlay dtc; do
	command -v "$tool" >/dev/null || { echo "Missing tool: $tool" >&2; exit 1; }
done

check_prop() {
	actual=$(fdtget -t "$1" "$2" / "$3")
	if [ "$actual" != "$4" ]; then
		printf '%s: expected %s = %s, got %s\n' "$2" "$3" "$4" "$actual" >&2
		exit 1
	fi
}

check_prop s "$base" compatible 'qcom,sdmmagpie'
check_prop x "$base" qcom,msm-id '16d 0'
check_prop x "$base" qcom,board-id '0 0'
mkdir -p "$out"
tmp=$(mktemp -d "$out/.surya-audit.XXXXXX")
trap 'rm -rf -- "$tmp"' EXIT
case "$overlay" in
	*.dts)
		dtc -I dts -O dtb -o "$tmp/idp.dtbo" "$overlay"
		overlay=$tmp/idp.dtbo
		;;
esac
check_prop x "$overlay" qcom,msm-id '16d 0'
check_prop x "$overlay" qcom,board-id '22 0'
fdtoverlay -i "$base" -o "$tmp/stock-surya-merged.dtb" "$overlay"
merged=$tmp/stock-surya-merged.dtb
check_prop x "$merged" qcom,msm-id '16d 0'
# Overlay-root properties select a DTBO; fdtoverlay only applies fragments.
merged_board=$(fdtget -t x "$merged" / qcom,board-id)
case "$merged_board" in
	'0 0'|'22 0') ;;
	*) printf 'Unexpected merged board ID: %s\n' "$merged_board" >&2; exit 1 ;;
esac
dtc -I dtb -O dts -o "$tmp/stock-surya-merged.dts" "$merged"
{
	printf 'Offline reference only; not a live FDT or a boot image.\n\n'
	printf 'Merged board ID: %s\n' "$merged_board"
	printf 'TLMM reg: '
	fdtget -t x "$merged" /soc/pinctrl@3400000 reg
	printf 'TLMM interrupts: '
	fdtget -t x "$merged" /soc/pinctrl@3400000 interrupts
	printf '\nFixed reserved memory (hex cells):\n'
	fdtget -l "$merged" /reserved-memory | while IFS= read -r node; do
		if reg=$(fdtget -t x "$merged" "/reserved-memory/$node" reg 2>/dev/null); then
			printf '%s\t%s\n' "$node" "$reg"
		fi
	done
} > "$tmp/stock-surya-summary.txt"
for name in stock-surya-merged.dtb stock-surya-merged.dts stock-surya-summary.txt; do
	mv -- "$tmp/$name" "$out/$name"
done
printf 'Offline audit written to %s\n' "$out"
