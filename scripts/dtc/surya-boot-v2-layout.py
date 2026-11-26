#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Read a local Android boot v2 image without unpacking or modifying it."""
import argparse
import struct
from pathlib import Path


def inspect(path):
    with path.open('rb') as image:
        header = image.read(1660)
        if len(header) != 1660 or header[:8] != b'ANDROID!':
            raise ValueError('not an Android boot image')
        kernel, ramdisk, second = (struct.unpack_from('<I', header, i)[0]
                                   for i in (8, 16, 24))
        page, version = struct.unpack_from('<II', header, 36)
        recovery, recovery_offset, header_size, dtb, address = struct.unpack_from(
            '<IQIIQ', header, 1632)
        if version != 2 or header_size != 1660 or page < header_size or page & (page - 1):
            raise ValueError('unsupported boot v2 header or page size')
        if not kernel or not dtb or second or recovery or recovery_offset:
            raise ValueError('expected a kernel and separate DTB without second stage/recovery DTBO')

        def align(size):
            return (size + page - 1) & -page

        ramdisk_offset = page + align(kernel)
        dtb_offset = ramdisk_offset + align(ramdisk)
        if path.stat().st_size < dtb_offset + dtb:
            raise ValueError('truncated DTB section')
        image.seek(page)
        if image.read(3) != b'\x1f\x8b\x08':
            raise ValueError('expected gzip at the kernel payload start')
        image.seek(dtb_offset)
        magic, total = struct.unpack('>II', image.read(8))
        if magic != 0xd00dfeed or total != dtb or total < 40:
            raise ValueError('DTB section is not one FDT of the declared size')
        return {
            'page_size': page, 'kernel_offset': page, 'kernel_size': kernel,
            'ramdisk_offset': ramdisk_offset, 'ramdisk_size': ramdisk,
            'dtb_offset': dtb_offset, 'dtb_size': dtb, 'dtb_load_address': address,
        }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path)
    args = parser.parse_args()
    try:
        for key, value in inspect(args.image).items():
            print(f'{key}: {value:#x}')
    except (OSError, ValueError, struct.error) as error:
        parser.exit(1, f'{error}\n')


if __name__ == '__main__':
    main()
