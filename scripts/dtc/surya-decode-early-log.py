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
        if length == ecc_capacity and cursor < length:
            return data[cursor:length] + data[:cursor]
        if cursor == length:
            return data[:length]
        if cursor < length:
            return data[length - cursor:length] + data[:length - cursor]
        raise ValueError('anchor cursor beyond length')
    if signature != SIGNATURE:
        raise ValueError('earlycon signature not found')
    if not length or length > len(data) or cursor >= len(data):
        raise ValueError('empty or invalid earlycon ring')
    if length == ecc_capacity and cursor < length:
        return data[cursor:length] + data[:cursor]
    if length < len(data):
        if cursor > length:
            raise ValueError('inconsistent cursor in partially filled ring')
        if cursor == length:
            return data[:length]
        # 4.14-era readers relocate the old prefix for cursor < length
        # (mid-flush torn header); reproduce what recovery would show.
        return data[length - cursor:length] + data[:length - cursor]
    return data[cursor:] + data[:cursor]


def scan(blob):
    """Find earlycon anchors inside an arbitrary raw region dump."""
    for off in range(0, len(blob) - 20, 4):
        sig, cursor, length, gen, csum = struct.unpack_from('<IIIII', blob, off)
        if sig != ANCHOR_SIGNATURE or csum != (
                ANCHOR_SIGNATURE ^ gen ^ cursor ^ length):
            continue
        if not length or length > CAPACITY or cursor >= CAPACITY:
            continue
        base = off - (WINDOW - 128)
        if base < 0 or base + WINDOW > len(blob):
            continue
        data = blob[base + 12:base + 12 + CAPACITY]
        if len(data) < length:
            continue
        if length == CAPACITY and cursor < length:
            yield base, data[cursor:length] + data[:cursor]
        elif cursor <= length:
            if cursor == length:
                yield base, data[:length]
            else:
                yield base, data[length - cursor:length] + data[:length - cursor]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('dump', type=Path)
    parser.add_argument('--scan', action='store_true',
                        help='search a larger raw region dump for anchors')
    args = parser.parse_args()
    blob = args.dump.read_bytes()
    if not args.scan and len(blob) == WINDOW:
        try:
            sys.stdout.buffer.write(decode(blob))
        except ValueError as exc:
            parser.exit(1, f'{exc}\n')
        return
    found = False
    for base, log in scan(blob):
        found = True
        sys.stdout.buffer.write(f'--- anchor at region+0x{base:x} '
                                f'(phys base guess) ---\n'.encode())
        sys.stdout.buffer.write(log)
    if not found:
        parser.exit(1, 'no valid earlycon anchor in region dump\n')


if __name__ == '__main__':
    main()
