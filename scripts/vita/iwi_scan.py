#!/usr/bin/env python3
"""Census of the shipped .iwi images inside the game's .iwd archives.

Reads each image's 28-byte header to tally formats, dimensions and payload size,
which is what decides the GXM texture mapping and the VRAM budget.

  python iwi_scan.py "<cod4>/main"
"""

import os
import struct
import sys
import zipfile
from collections import Counter

FORMATS = {
    1: "A8R8G8B8", 2: "X8R8G8B8", 3: "A8L8", 4: "L8", 5: "A8",
    6: "wavelet A8R8G8B8", 7: "wavelet X8R8G8B8", 8: "wavelet A8L8",
    9: "wavelet L8", 10: "wavelet A8",
    11: "DXT1", 12: "DXT3", 13: "DXT5",
}


def main():
    root = sys.argv[1]
    archives = sorted(f for f in os.listdir(root) if f.lower().endswith(".iwd"))

    formats = Counter()
    payload = Counter()
    sizes = Counter()
    widths = []
    total = 0
    cubemaps = 0
    volumes = 0
    nomips = 0
    picmip_bytes = [0, 0, 0, 0]

    for name in archives:
        with zipfile.ZipFile(os.path.join(root, name)) as zf:
            for entry in zf.infolist():
                if not entry.filename.lower().endswith(".iwi"):
                    continue
                with zf.open(entry) as f:
                    head = f.read(28)
                if len(head) < 28 or head[:3] != b"IWi":
                    continue

                fmt, flags = head[4], head[5]
                w, h, d = struct.unpack_from("<3h", head, 6)
                total += 1
                formats[FORMATS.get(fmt, "format %d" % fmt)] += 1
                payload[FORMATS.get(fmt, "format %d" % fmt)] += entry.file_size
                widths.append(max(w, h))
                sizes[1 << max(w, h).bit_length() - 1] += 1

                # the sizes the file itself records for each picmip level
                for level, size in enumerate(struct.unpack_from("<4i", head, 12)):
                    picmip_bytes[level] += size

                if flags & 0x2:         # the engine reports a mip count of 1 for these
                    nomips += 1
                if flags & 0x4:         # six faces
                    cubemaps += 1
                if d > 1:
                    volumes += 1

    print("=== %d images across %d archives ===\n" % (total, len(archives)))
    print("%-20s %7s %10s" % ("format", "count", "MB"))
    for fmt, count in formats.most_common():
        print("%-20s %7d %10.1f" % (fmt, count, payload[fmt] / 1048576.0))
    print("%-20s %7d %10.1f" % ("total", total, sum(payload.values()) / 1048576.0))

    widths.sort()
    print("\nlargest dimension: max %d, median %d" % (widths[-1], widths[len(widths) // 2]))
    print("cubemaps: %d, volumes: %d, images without mipmaps: %d" % (cubemaps, volumes, nomips))

    print("\n%-8s %10s %8s" % ("picmip", "MB", "of full"))
    for level, size in enumerate(picmip_bytes):
        mb = size / 1048576.0
        share = 100.0 * size / picmip_bytes[0] if picmip_bytes[0] else 0.0
        print("%-8d %10.1f %7.1f%%" % (level, mb, share))

    print("\n%-10s %s" % ("size", "count (by larger edge, rounded down to a power of two)"))
    for size in sorted(sizes):
        print("%-10d %d" % (size, sizes[size]))


if __name__ == "__main__":
    main()
