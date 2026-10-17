#!/usr/bin/env python3
"""Pack the Wonder data a Windows PC needs into one zip (docs/nvn/WINDOWS-PORT.md, W0).

Extracted into the repository root on the PC, it recreates private/wonder as the Mac has it:

    private/wonder/exefs/{main,sdk,main.npdm}   the executables, to recompile on the PC (python -m recomp)
    private/wonder/analysis/{main,sdk}.bin      the modules as loaded (tools/nso2bin.py output)
    private/wonder/extract/romfs/...            the RomFS (3.5 GB)
    private/wonder/spirv/<hash>.spv             the offline shader translation
    private/wonder/spirv/results.tsv            the harness's records (storage buffer offsets; the first PC zip lacked it)
    private/wonder/fonts/shared_5.ttf           the button glyphs

The generated C++ (private/wonder/gen, 1.4 GB) is not packed: recompiling on the PC takes about 6 minutes and
gives the same files. Everything comes from the owner's own dump; never share the result.

Usage: pack_pc_wonder.py [--root private/wonder] [--out private/wonder/pc/Wonder-pc-data.zip] [--list]
"""
import argparse
import os
import sys
import zipfile

sys.path.insert(0, os.path.dirname(__file__))
from pack_phone_wonder import entries  # noqa: E402  (analysis, romfs, spirv, font)


def pc_entries(root):
    for f in ("main", "sdk"):
        p = os.path.join(root, "exefs", f)
        if not os.path.isfile(p):
            sys.exit(f"missing {p}")
        yield p, "exefs/" + f
    npdm = os.path.join(root, "exefs", "main.npdm")
    if not os.path.isfile(npdm):
        npdm = os.path.join(root, "analysis", "main.npdm")
    if not os.path.isfile(npdm):
        sys.exit(f"missing {root}/exefs/main.npdm and {root}/analysis/main.npdm")
    yield npdm, "exefs/main.npdm"
    yield from entries(root)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default="private/wonder")
    ap.add_argument("--out", default="private/wonder/pc/Wonder-pc-data.zip")
    ap.add_argument("--list", action="store_true")
    a = ap.parse_args()
    files = list(pc_entries(a.root))
    total = sum(os.path.getsize(p) for p, _ in files)
    print(f"{len(files)} files, {total / 2**30:.2f} GB")
    if a.list:
        return
    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    with zipfile.ZipFile(a.out, "w", zipfile.ZIP_STORED, allowZip64=True) as z:
        for k, (p, name) in enumerate(files):
            z.write(p, "private/wonder/" + name)
            if k % 4000 == 0:
                print(f"  {k}/{len(files)}", flush=True)
    print(f"wrote {a.out} ({os.path.getsize(a.out) / 2**30:.2f} GB)")


if __name__ == "__main__":
    main()
