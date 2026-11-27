#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Decode an already captured Surya earlycon window; does not access RAM."""
import argparse
import struct
import sys
from pathlib import Path

WINDOW = 0x40000
SIGNATURE = 0x43474244
ANCHOR_SIGNATURE = 0x45434C32
ANCHOR_OFFSET = WINDOW - 128
# Data capacity published by the earlycon driver (header + anchor removed,
# rounded down to whole 128-byte ECC blocks).
CAPACITY = (WINDOW - 12 - 128) // 128 * 128


def anchor(raw):
    sig, cursor, length, generation, csum = struct.unpack_from('<IIIII', raw,
                                                                ANCHOR_OFFSET)
    if sig != ANCHOR_SIGNATURE or csum != (
            ANCHOR_SIGNATURE ^ generation ^ cursor ^ length):
        return None
    if length > CAPACITY or cursor >= max(length, 1) or cursor >= CAPACITY:
        return None
    return cursor, length, generation


def decode(raw):
    if len(raw) != WINDOW:
        raise ValueError('expected exactly 256 KiB from the earlycon window')
    signature, cursor, length = struct.unpack_from('<III', raw)
    data = raw[12:]
    # The ECC writer only uses whole 128-byte blocks. A wrapped ECC
    # record can be distinguished from a partial legacy record by its cursor.
    ecc_capacity = CAPACITY
    if signature != SIGNATURE or not length:
        # A later ramoops driver may have zapped the 12-byte header
        # (start, size -> 0) without touching the ring or the anchor.
        recovery = anchor(raw)
        if recovery is None:
            raise ValueError('earlycon header invalid and no anchor record')
        cursor, length, generation = recovery
        if not length:
            raise ValueError(
                f'earlycon came up (anchor generation {generation}) but no '
                'output was flushed before the reset')
        if length == ecc_capacity:
            return data[cursor:length] + data[:cursor]
        return data[:length]
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
