#!/usr/bin/env python3
"""Strip the top mip levels from streamed .iwi textures and repack the .iwd archives.

The IWI v8 header is the engine's GfxImageFileHeader: magic 'IWi', version, format,
flags, u16 dims[3], then fileSizeForPicmip[4] -- the file size the loader reads for
each picmip level. Mips are stored smallest-first, so dropping the top k levels is a
truncation at fileSizeForPicmip[k] plus a header rewrite; the result is byte-exact
with what the engine itself would keep at r_picmip k. Files the loader refuses to
picmip (flags & 3, or min dimension < 32) pass through untouched, as does every
non-image entry.

  python iwi_strip.py "<cod4>/main" <outdir> [--max-dim 512]
"""

import argparse
import os
import struct
import zipfile

HEADER = struct.Struct("<3sBBB3H4I")


def strip_iwi(data, max_dim):
    if len(data) < 28 or data[:3] != b"IWi" or data[3] != 6:
        return data, 0
    magic, version, fmt, flags, w, h, d, s0, s1, s2, s3 = HEADER.unpack_from(data)
    sizes = [s0, s1, s2, s3]
    if (flags & 3) or d > 1 or min(w, h) < 32:
        return data, 0

    levels = 0
    while (max(w >> levels, h >> levels) > max_dim and levels < 3
           and sizes[levels + 1] < sizes[levels]
           and min(w >> (levels + 1), h >> (levels + 1)) >= 32):
        levels += 1
    if not levels:
        return data, 0

    new_sizes = [sizes[min(i + levels, 3)] for i in range(4)]
    nw, nh = max(w >> levels, 1), max(h >> levels, 1)
    header = HEADER.pack(magic, version, fmt, flags, nw, nh, d, *new_sizes)
    out = header + data[28:sizes[levels]]
    assert len(out) == new_sizes[0], "size table does not match truncation"
    return out, len(data) - len(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("source")
    ap.add_argument("outdir")
    ap.add_argument("--max-dim", type=int, default=512)
    args = ap.parse_args()

    os.makedirs(args.outdir, exist_ok=True)
    names = sorted(n for n in os.listdir(args.source) if n.lower().endswith(".iwd"))
    total_before = total_after = 0

    for name in names:
        src_path = os.path.join(args.source, name)
        dst_path = os.path.join(args.outdir, name)
        stripped = images = 0
        with zipfile.ZipFile(src_path) as src, \
             zipfile.ZipFile(dst_path, "w", zipfile.ZIP_DEFLATED) as dst:
            for entry in src.infolist():
                data = src.read(entry)
                if entry.filename.lower().endswith(".iwi"):
                    images += 1
                    data, saved = strip_iwi(data, args.max_dim)
                    if saved:
                        stripped += 1
                dst.writestr(entry.filename, data)
        before, after = os.path.getsize(src_path), os.path.getsize(dst_path)
        total_before += before
        total_after += after
        print("%-28s %4d/%4d iwi stripped  %7.1f MB -> %7.1f MB"
              % (name, stripped, images, before / 1048576.0, after / 1048576.0))

    print("\n%.2f GB -> %.2f GB (saved %.2f GB)"
          % (total_before / 1073741824.0, total_after / 1073741824.0,
             (total_before - total_after) / 1073741824.0))


if __name__ == "__main__":
    main()
