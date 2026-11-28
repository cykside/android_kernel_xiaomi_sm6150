#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Build an unsigned Surya boot v2 fixture for offline inspection only."""
import argparse
import hashlib
import runpy
import struct
from pathlib import Path


def repack(reference, kernel, dtb, output):
    tools = runpy.run_path(str(Path(__file__).with_name('surya-boot-v2-layout.py')))
    layout = tools['inspect'](reference)
    plan = tools['replacement_plan'](layout, kernel, dtb)
    page = layout['page_size']
    with reference.open('rb') as source:
        # Keep the header fields, not arbitrary padding or trailing metadata.
        header = bytearray(source.read(1660))
        source.seek(layout['ramdisk_offset'])
        ramdisk = source.read(layout['ramdisk_size'])
    payloads = kernel.read_bytes(), ramdisk, dtb.read_bytes()
    if tuple(map(len, payloads)) != (plan['kernel_size'], plan['ramdisk_size'], plan['dtb_size']):
        raise ValueError('input size changed while reading')
    struct.pack_into('<I', header, 8, plan['kernel_size'])
    struct.pack_into('<I', header, 1648, plan['dtb_size'])
    digest = hashlib.sha1(usedforsecurity=False)
    for payload in (*payloads[:2], b'', b'', payloads[2]):
        digest.update(payload)
        digest.update(struct.pack('<I', len(payload)))
    header[576:608] = digest.digest().ljust(32, b'\0')
    with output.open('xb') as fixture:
        for section in (header, *payloads):
            fixture.write(section)
            fixture.write(bytes(-len(section) % page))
    if output.stat().st_size != plan['padded_payload_end']:
        raise ValueError('output differs from dry-run size')
    result = tools['inspect'](output)
    for name, value in result.items():
        if value != plan[name]:
            raise ValueError(f'output disagrees with dry run: {name}')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('reference', 'kernel', 'dtb', 'output'):
        parser.add_argument(name, type=Path)
    args = parser.parse_args()
    if not args.output.name.endswith('.unsigned-fixture.img'):
        parser.error('output must end in .unsigned-fixture.img')
    try:
        repack(args.reference, args.kernel, args.dtb, args.output)
    except (OSError, ValueError, struct.error) as error:
        parser.exit(1, f'{error}\n')
    print(f'{args.output}: unsigned offline fixture, no AVB metadata; do not boot or flash')


if __name__ == '__main__':
    main()
