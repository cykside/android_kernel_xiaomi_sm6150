#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Build a temporary host fdtoverlay that prefers exact node names."""
import subprocess
import sys
import tempfile
from pathlib import Path

src = Path(__file__).resolve().parent
lib = src / 'libfdt'
output = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory() as directory:
    work = Path(directory)
    (work / 'version_gen.h').write_text('#define DTC_VERSION "Surya diagnostic"\n')
    original = (lib / 'fdt_ro.c').read_text()
    begin = original.index('int fdt_subnode_offset_namelen(')
    end = original.index('\nint fdt_subnode_offset(', begin)
    function = original[begin:end]
    function = function.replace('int depth;', 'int depth, fallback = -FDT_ERR_NOTFOUND;')
    function = function.replace('))\n\t\t\treturn offset;', ''')) {
			int len;

			fdt_get_name(fdt, offset, &len);
			if (len == namelen)
				return offset;
			if (fallback < 0)
				fallback = offset;
		}''')
    function = function.replace('return -FDT_ERR_NOTFOUND;', 'return fallback;')
    if 'len == namelen' not in function:
        raise RuntimeError('unexpected libfdt source layout')
    patched = work / 'fdt_ro.c'
    patched.write_text(original[:begin] + function + original[end:])
    sources = [p for p in sorted(lib.glob('*.c')) if p.name != 'fdt_ro.c']
    for ro, target in ((lib / 'fdt_ro.c', output.with_name(output.name + '-control')),
                       (patched, output)):
        subprocess.run(['cc', '-O2', '-I', str(work), '-I', str(lib), '-I', str(src),
                        str(src / 'fdtoverlay.c'), str(src / 'util.c'), str(ro),
                        *map(str, sources), '-o', str(target)], check=True)
