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


def supply_users(fdt, providers, node=0, enabled=True):
    status = prop(fdt, node, 'status')
    enabled = enabled and (status is None or status in (b'okay\0', b'ok\0'))
    offset = fdt.first_property_offset(node, libfdt.QUIET_NOTFOUND)
    while offset >= 0:
        value = fdt.get_property_by_offset(offset)
        if value.name.endswith('-supply') and len(value) == 4:
            handle, = cells(value)
            if handle in providers:
                state = 'available' if enabled else 'disabled'
                print(f'  {state}: {fdt.get_path(node)} {value.name} -> {providers[handle]}')
        offset = fdt.next_property_offset(offset, libfdt.QUIET_NOTFOUND)
    child = fdt.first_subnode(node, libfdt.QUIET_NOTFOUND)
    while child >= 0:
        supply_users(fdt, providers, child, enabled)
        child = fdt.next_subnode(child, libfdt.QUIET_NOTFOUND)


def report(path):
    fdt = libfdt.Fdt(Path(path).read_bytes())
    print(f'\n{Path(path).name}: resolved UFS providers')
    providers = {}
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
                target_path = fdt.get_path(target)
                print(f'  {ref}: {target_path}')
                if ref.endswith('-supply'):
                    providers[handle] = target_path
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

    print('  UFS supply consumers (DT status, not runtime votes):')
    supply_users(fdt, providers)


if __name__ == '__main__':
    for dtb in sys.argv[1:]:
        report(dtb)
