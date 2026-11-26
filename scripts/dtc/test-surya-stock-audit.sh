#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
tmp=$(mktemp -d)
trap 'rm -rf -- "$tmp"' 0

# Synthetic fixture: no stock firmware downloads or device access.
cat > "$tmp/base.dts" <<'DTS'
/dts-v1/;
/ {
	#address-cells = <1>;
	#size-cells = <1>;
	compatible = "qcom,sdmmagpie";
	qcom,msm-id = <0x16d 0>;
	qcom,board-id = <0 0>;
	reserved-memory {
		#address-cells = <1>;
		#size-cells = <1>;
		ranges;
		buffer@90000000 { reg = <0x90000000 0x1000>; };
	};
	soc {
		#address-cells = <1>;
		#size-cells = <1>;
		tlmm: pinctrl@3400000 {
			reg = <0x3400000 0xdc2000>;
		};
	};
};
DTS
cat > "$tmp/idp.dts" <<'DTS'
/dts-v1/;
/plugin/;
/ {
	qcom,msm-id = <0x16d 0>;
	qcom,board-id = <0x22 0>;
};
&tlmm { interrupts = <0 208 4>; };
DTS

dtc -q -@ -I dts -O dtb -o "$tmp/base.dtb" "$tmp/base.dts"
dtc -q -I dts -O dtb -o "$tmp/idp.dtbo" "$tmp/idp.dts"
for overlay in "$tmp/idp.dts" "$tmp/idp.dtbo"; do
	"$here/surya-stock-audit.sh" "$tmp/base.dtb" "$overlay" "$tmp/report"
	test "$(fdtget -t x "$tmp/report/stock-surya-merged.dtb" / qcom,board-id)" = '0 0'
	grep -Fx 'TLMM interrupts: 0 d0 4' "$tmp/report/stock-surya-summary.txt"
	grep -F '90000000 1000' "$tmp/report/stock-surya-summary.txt"
done

sed 's/0x22/0x23/g' "$tmp/idp.dts" > "$tmp/wrong-id.dts"
if "$here/surya-stock-audit.sh" "$tmp/base.dtb" "$tmp/wrong-id.dts" "$tmp/rejected" > "$tmp/error" 2>&1; then
	echo 'Unexpected success with a non-Surya overlay' >&2
	exit 1
fi
grep -F 'expected qcom,board-id = 22 0, got 23 0' "$tmp/error"
test ! -e "$tmp/rejected/stock-surya-merged.dtb"
