#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
checker=$(dirname "$0")/surya-overlay-check.sh
cat > "$work/base.dts" <<'DTS'
/dts-v1/;
/ { consumer: device { status = "disabled"; }; };
DTS
cat > "$work/overlay.dts" <<'DTS'
/dts-v1/;
/plugin/;
&consumer { status = "okay"; };
DTS
dtc -@ -I dts -O dtb -o "$work/base.dtb" "$work/base.dts"
dtc -I dts -O dtb -o "$work/no-symbols.dtb" "$work/base.dts"
dtc -@ -I dts -O dtb -o "$work/overlay.dtbo" "$work/overlay.dts"
sh "$checker" "$work/base.dtb" "$work/overlay.dtbo" > "$work/result"
grep -Fx 'Missing external symbols: 0/1' "$work/result"
if sh "$checker" "$work/no-symbols.dtb" "$work/overlay.dtbo" "$work/base.dtb" > "$work/result" 2>&1; then
	echo 'Unexpected overlay success without the required symbol' >&2
	exit 1
fi
grep -Fx 'Missing external symbols: 1/1' "$work/result"
printf 'consumer\t/device\n' | grep -Fx -f - "$work/result"
