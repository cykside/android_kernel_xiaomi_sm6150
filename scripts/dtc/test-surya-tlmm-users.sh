#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
cat > "$work/input.dts" <<'DTS'
/dts-v1/;
/ {
	t: tlmm {
		interrupt-controller;
		#address-cells = <0>;
		#interrupt-cells = <2>;
	};
	g: other-intc {
		interrupt-controller;
		#address-cells = <0>;
		#interrupt-cells = <3>;
	};
	consumer {
		interrupts-extended = <&g 0 60 4>, <&t 9 2>;
	};
};
DTS
dtc -I dts -O dtb -o "$work/input.dtb" "$work/input.dts"
"${PYTHON:-python3}" "$(dirname "$0")/surya-tlmm-users.py" "$work/input.dtb" /tlmm > "$work/report"
printf 'available\t/consumer\tinterrupts-extended\t9 2\n' | grep -Fx -f - "$work/report"
test "$(wc -l < "$work/report")" -eq 2
