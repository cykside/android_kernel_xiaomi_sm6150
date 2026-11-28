#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compare fixed reserved-memory ranges in two flattened device trees."""

import argparse
import struct
from pathlib import Path

import libfdt


def numbers(value):
    return [item[0] for item in struct.iter_unpack('>I', value)]


def integer(cells):
    value = 0
    for cell in cells:
        value = (value << 32) | cell
    return value


def ranges(path):
    fdt = libfdt.Fdt(path.read_bytes())
    root = fdt.path_offset('/reserved-memory')
    address_cells, = numbers(fdt.getprop(root, '#address-cells'))
    size_cells, = numbers(fdt.getprop(root, '#size-cells'))
    if address_cells not in (1, 2) or size_cells not in (1, 2):
        raise ValueError(f'{path}: unsupported reserved-memory cell widths')
    width = address_cells + size_cells
    node = fdt.first_subnode(root, libfdt.QUIET_NOTFOUND)
    fixed = []
    while node >= 0:
        status = fdt.getprop(node, 'status', libfdt.QUIET_NOTFOUND)
        reg = fdt.getprop(node, 'reg', libfdt.QUIET_NOTFOUND)
        if (isinstance(status, int) or status in (b'okay\0', b'ok\0')) and not isinstance(reg, int):
            cells = numbers(reg)
            name = fdt.get_name(node)
            if len(cells) % width:
                raise ValueError(f'{path}: malformed reg in {name}')
            for i in range(0, len(cells), width):
                start = integer(cells[i:i + address_cells])
                size = integer(cells[i + address_cells:i + width])
                if size:
                    fixed.append((start, start + size, name))
        node = fdt.next_subnode(node, libfdt.QUIET_NOTFOUND)
    return sorted(fixed)


def describe(entry):
    start, end, name = entry
    return f'{name}: [0x{start:x}, 0x{end:x})'


def compare(stock, port):
    stock_ranges = {(start, end) for start, end, _ in stock}
    port_ranges = {(start, end) for start, end, _ in port}
    for entry in stock:
        if entry[:2] not in port_ranges:
            yield 'stock-only range: ' + describe(entry)
    for entry in port:
        if entry[:2] not in stock_ranges:
            yield 'port-only range: ' + describe(entry)
    for old in stock:
        for new in port:
            if old[:2] != new[:2] and max(old[0], new[0]) < min(old[1], new[1]):
                yield f'partial overlap: stock {describe(old)}; port {describe(new)}'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('stock', type=Path)
    parser.add_argument('port', type=Path)
    args = parser.parse_args()
    print('Fixed DT reservations only; excludes dynamic allocations and bootloader fixups.')
    for row in compare(ranges(args.stock), ranges(args.port)):
        print(row)


if __name__ == '__main__':
    main()
