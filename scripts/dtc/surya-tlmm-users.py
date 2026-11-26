#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Inventory DTB users of a TLMM provider without accessing hardware."""

import argparse
import re
import struct
from pathlib import Path

import libfdt


def cells(value):
    return [cell[0] for cell in struct.iter_unpack('>I', value)]


def children(fdt, parent):
    node = fdt.first_subnode(parent, libfdt.QUIET_NOTFOUND)
    while node >= 0:
        yield node
        node = fdt.next_subnode(node, libfdt.QUIET_NOTFOUND)


def properties(fdt, node):
    offset = fdt.first_property_offset(node, libfdt.QUIET_NOTFOUND)
    while offset >= 0:
        prop = fdt.get_property_by_offset(offset)
        yield prop.name, prop
        offset = fdt.next_property_offset(offset, libfdt.QUIET_NOTFOUND)


def inventory(fdt, tlmm):
    provider = fdt.get_phandle(tlmm)
    prefix = fdt.get_path(tlmm) + '/'

    def walk(node, enabled, parent_irq):
        props = dict(properties(fdt, node))
        enabled = enabled and props.get('status', b'okay\0') in (b'okay\0', b'ok\0')
        if 'interrupt-parent' in props:
            parent_irq, = cells(props['interrupt-parent'])
        for name, value in sorted(props.items()):
            if re.fullmatch(r'pinctrl-\d+', name):
                for phandle in cells(value):
                    offset = fdt.node_offset_by_phandle(phandle, libfdt.QUIET_NOTFOUND)
                    if offset >= 0 and fdt.get_path(offset).startswith(prefix):
                        yield node, enabled, name, fdt.get_path(offset)[len(prefix):]
            elif name in ('gpio', 'gpios') or name.endswith(('-gpio', '-gpios')):
                if len(value) % 4:
                    continue
                spec = iter(cells(value))
                for phandle in spec:
                    if not phandle:
                        continue
                    offset = fdt.node_offset_by_phandle(phandle, libfdt.QUIET_NOTFOUND)
                    if offset < 0:
                        break
                    count = fdt.getprop(offset, '#gpio-cells', libfdt.QUIET_NOTFOUND)
                    if isinstance(count, int):
                        break
                    argc, = cells(count)
                    args = [next(spec, None) for _ in range(argc)]
                    if phandle == provider:
                        yield node, enabled, name, ' '.join(map(str, args))
            elif name == 'interrupts' and provider and parent_irq == provider:
                yield node, enabled, name, ' '.join(map(str, cells(value)))
        for child in children(fdt, node):
            yield from walk(child, enabled, parent_irq)

    for node, enabled, prop, target in walk(0, True, None):
        state = 'available' if enabled else 'disabled'
        yield f'{state}\t{fdt.get_path(node)}\t{prop}\t{target}'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('dtb', type=Path)
    parser.add_argument('tlmm_path', help='node path in the input DTB')
    args = parser.parse_args()
    fdt = libfdt.Fdt(args.dtb.read_bytes())
    tlmm = fdt.path_offset(args.tlmm_path)
    print('status\tnode\tproperty\tTLMM target or cells')
    for line in sorted(inventory(fdt, tlmm)):
        print(line)


if __name__ == '__main__':
    main()
