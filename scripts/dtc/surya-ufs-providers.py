#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Resolve Surya UFS supply and NVMEM references in compiled DTBs."""
import struct
import sys
from pathlib import Path

import libfdt


def prop(fdt, node, name):
    result = fdt.getprop(node, name, libfdt.QUIET_NOTFOUND)
    return None if isinstance(result, int) else result


def cells(value):
    return [item[0] for item in struct.iter_unpack('>I', value)]


def report(path):
    fdt = libfdt.Fdt(Path(path).read_bytes())
    print(f'\n{Path(path).name}: resolved UFS providers')
    for name, refs in (
        ('ufshc@1d84000', ('vdd-hba-supply', 'vcc-supply', 'vccq2-supply',
                            'qcom,vddp-ref-clk-supply', 'nvmem-cells')),
        ('ufsphy_mem@1d87000', ('vdda-phy-supply', 'vdda-pll-supply')),
    ):
        node = fdt.path_offset('/soc/' + name, libfdt.QUIET_NOTFOUND)
        print(name)
        if node < 0:
            print('  node absent')
            continue
        for ref in refs:
            value = prop(fdt, node, ref)
            if value is None:
                print(f'  {ref}: absent')
                continue
            for handle in cells(value):
                target = fdt.node_offset_by_phandle(handle, libfdt.QUIET_NOTFOUND)
                if target < 0:
                    print(f'  {ref}: unresolved phandle {handle:#x}')
                    continue
                print(f'  {ref}: {fdt.get_path(target)}')
                names = ('reg',) if ref == 'nvmem-cells' else (
                    'regulator-min-microvolt', 'regulator-max-microvolt',
                    'qcom,init-voltage')
                for field in names:
                    data = prop(fdt, target, field)
                    if data is not None:
                        values = cells(data)
                        text = ' '.join(hex(i) if field == 'reg' else str(i)
                                        for i in values)
                        print(f'    {field}: {text}')


if __name__ == '__main__':
    for dtb in sys.argv[1:]:
        report(dtb)
