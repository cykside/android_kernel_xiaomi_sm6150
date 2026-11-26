#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
script_dir=$(dirname "$0")
cat > "$work/test.dts" <<'DTS'
/dts-v1/;
/ {
	regulator {
		phandle = <0x42>;
		regulator-min-microvolt = <1144000>;
		regulator-max-microvolt = <1256000>;
	};
	qfprom {
		#address-cells = <1>;
		#size-cells = <1>;
		boot-config@6070 {
			phandle = <0x91>;
			reg = <0x6070 4>;
		};
	};
	soc {
		#address-cells = <1>;
		#size-cells = <1>;
		ufshc@1d84000 {
			reg = <0x1d84000 0x3000>;
			qcom,vddp-ref-clk-supply = <0x42>;
			nvmem-cells = <0x91>;
		};
		ufsphy_mem@1d87000 {
			reg = <0x1d87000 0xddc>;
			vdda-pll-supply = <0x42>;
		};
	};
};
DTS
dtc -I dts -O dtb -o "$work/test.dtb" "$work/test.dts"
"${PYTHON:-python3}" "$script_dir/surya-ufs-providers.py" "$work/test.dtb" > "$work/report"
grep -Fx '  qcom,vddp-ref-clk-supply: /regulator' "$work/report"
grep -Fx '  vdda-pll-supply: /regulator' "$work/report"
grep -Fx '    regulator-min-microvolt: 1144000' "$work/report"
grep -Fx '    regulator-max-microvolt: 1256000' "$work/report"
grep -Fx '  nvmem-cells: /qfprom/boot-config@6070' "$work/report"
grep -Fx '    reg: 0x6070 0x4' "$work/report"
grep -Fx '  vcc-supply: absent' "$work/report"
