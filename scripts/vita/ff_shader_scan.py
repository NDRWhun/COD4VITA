#!/usr/bin/env python3
"""Count unique SM3 shaders across CoD4 fastfiles.

Decompresses each .ff and scans the zone image for D3D9 shader token streams,
hashing each one the way the runtime will at CreateVertexShader time.

  python ff_shader_scan.py "<cod4>/zone/english" [dumpdir]

With a dumpdir, each unique stream is written there as <hash>.vs / <hash>.ps.
"""

import os
import struct
import sys
import zlib
from collections import defaultdict

VS_VERSION = 0xFFFE0300
PS_VERSION = 0xFFFF0300
END_TOKEN = 0x0000FFFF
MAX_DWORDS = 65536


# D3DSIO opcodes valid in SM3: arithmetic/flow 0..48, texture ops 64..97, plus PHASE
VALID_OPCODES = set(range(0, 49)) | set(range(64, 98)) | {0xFFFD}


def stream_length(buf, off):
    """DWORD count of the token stream at off, or None if it isn't well formed."""
    n = len(buf)
    p = off + 4
    while p + 4 <= n:
        tok = struct.unpack_from("<I", buf, p)[0]
        if tok == END_TOKEN:
            count = (p - off) // 4 + 1
            return count if count <= MAX_DWORDS else None
        opcode = tok & 0xFFFF
        if opcode == 0xFFFE:
            skip = 1 + ((tok >> 16) & 0x7FFF)
        else:
            if opcode not in VALID_OPCODES:
                return None
            skip = 1 + ((tok >> 24) & 0xF)
        p += skip * 4
        if (p - off) // 4 > MAX_DWORDS:
            return None
    return None


def fnv1a(data):
    h = 2166136261
    for b in data:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def scan(buf, shaders, per_file, dumpdir=None):
    n = len(buf)
    p = 0
    while p + 4 <= n:
        tok = struct.unpack_from("<I", buf, p)[0]
        if tok == VS_VERSION or tok == PS_VERSION:
            dwords = stream_length(buf, p)
            if dwords:
                blob = buf[p:p + dwords * 4]
                h = fnv1a(blob)
                kind = "vs" if tok == VS_VERSION else "ps"
                if h not in shaders:
                    shaders[h] = (kind, dwords)
                    if dumpdir:
                        with open(os.path.join(dumpdir, "%08x.%s" % (h, kind)), "wb") as f:
                            f.write(blob)
                per_file.add(h)
                p += dwords * 4
                continue
        p += 4


def main():
    zone = sys.argv[1]
    dumpdir = sys.argv[2] if len(sys.argv) > 2 else None
    if dumpdir:
        os.makedirs(dumpdir, exist_ok=True)

    files = sorted(f for f in os.listdir(zone) if f.endswith(".ff"))

    shaders = {}
    rows = []
    for name in files:
        path = os.path.join(zone, name)
        with open(path, "rb") as f:
            if f.read(8) != b"IWffu100":
                continue
            f.read(4)
            try:
                raw = zlib.decompress(f.read())
            except zlib.error as e:
                print("%-32s decompress failed: %s" % (name, e))
                continue

        per_file = set()
        before = len(shaders)
        scan(raw, shaders, per_file, dumpdir)
        rows.append((name, len(raw), len(per_file), len(shaders) - before))
        print("%-32s %7.1f MB  %4d shaders (%3d new)  running total %d"
              % (name, len(raw) / 1048576.0, len(per_file), len(shaders) - before, len(shaders)))

    vs = sum(1 for k, _ in shaders.values() if k == "vs")
    ps = len(shaders) - vs
    dwords = [d for _, d in shaders.values()]
    print("\n=== %d unique shaders: %d vs, %d ps ===" % (len(shaders), vs, ps))
    print("token stream dwords: min %d, median %d, max %d, total %d"
          % (min(dwords), sorted(dwords)[len(dwords) // 2], max(dwords), sum(dwords)))

    buckets = defaultdict(int)
    for _, d in shaders.values():
        buckets[min(d // 100 * 100, 1000)] += 1
    for b in sorted(buckets):
        print("  %5d-%-5d dwords: %d" % (b, b + 99, buckets[b]))


if __name__ == "__main__":
    main()
