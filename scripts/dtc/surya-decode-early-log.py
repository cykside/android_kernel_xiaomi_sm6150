#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Decode an already captured Surya earlycon window; does not access RAM."""
import argparse
import struct
import sys
from pathlib import Path

WINDOW = 0x40000
SIGNATURE = 0x43474244


def decode(raw):
    if len(raw) != WINDOW:
        raise ValueError('expected exactly 256 KiB from the earlycon window')
    signature, cursor, length = struct.unpack_from('<III', raw)
    data = raw[12:]
    # The ECC writer only uses whole 128-byte blocks. A wrapped ECC
    # record can be distinguished from a partial legacy record by its cursor.
    ecc_capacity = len(data) // 128 * 128
    if signature != SIGNATURE:
        raise ValueError('earlycon signature not found')
    if not length or length > len(data) or cursor >= len(data):
        raise ValueError('empty or invalid earlycon ring')
    if length == ecc_capacity and cursor < length:
        return data[cursor:length] + data[:cursor]
    if length < len(data):
        if cursor != length:
            raise ValueError('inconsistent cursor in partially filled ring')
        return data[:length]
    return data[cursor:] + data[:cursor]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('dump', type=Path)
    args = parser.parse_args()
    try:
        log = decode(args.dump.read_bytes())
    except (OSError, ValueError) as exc:
        parser.exit(1, f'{exc}\n')
    sys.stdout.buffer.write(log)


if __name__ == '__main__':
    main()
