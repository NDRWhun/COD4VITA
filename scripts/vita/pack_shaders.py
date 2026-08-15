#!/usr/bin/env python3
"""Pack the baked GXP blobs into one archive the runtime can look up by hash.

The runtime hashes a shader's SM3 bytecode at create time and asks for the blob
matching (hash, stage, alpha-test mode), so the index is sorted on that key and
searched with a binary search.

  python pack_shaders.py <gxpdir> <out.kgxp>

Layout: 16-byte header, then count 16-byte entries, then the blobs.
  header   magic 'KGXP', version, count, blob base offset
  entry    hash u32, stage u8, alphaTest u8, reserved u16, offset u32, size u32
"""

import argparse
import os
import struct
import sys

MAGIC = b"KGXP"
VERSION = 1
HEADER_SIZE = 16
ENTRY_SIZE = 16

STAGE_VERTEX = 0
STAGE_FRAGMENT = 1

ALPHA_SUFFIX = {"": 0, "a1": 1, "a2": 2, "a3": 3}


def parse_name(name):
    """<hash>.vs.gxp / <hash>.ps.gxp / <hash>.ps.a1.gxp -> (hash, stage, alphaTest)"""
    if not name.endswith(".gxp"):
        return None
    parts = name[:-len(".gxp")].split(".")
    if len(parts) < 2:
        return None

    try:
        shader_hash = int(parts[0], 16)
    except ValueError:
        return None

    if parts[1] == "vs":
        stage = STAGE_VERTEX
    elif parts[1] == "ps":
        stage = STAGE_FRAGMENT
    else:
        return None

    suffix = parts[2] if len(parts) > 2 else ""
    if suffix not in ALPHA_SUFFIX:
        return None
    return shader_hash, stage, ALPHA_SUFFIX[suffix]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("gxpdir")
    ap.add_argument("output")
    args = ap.parse_args()

    entries = []
    for name in sorted(os.listdir(args.gxpdir)):
        key = parse_name(name)
        if not key:
            continue
        with open(os.path.join(args.gxpdir, name), "rb") as f:
            blob = f.read()
        if blob[:4] != b"GXP\0":
            sys.exit("%s is not a GXP blob" % name)
        entries.append((key, blob))

    if not entries:
        sys.exit("no GXP blobs in %s" % args.gxpdir)

    entries.sort(key=lambda e: e[0])
    for i in range(1, len(entries)):
        if entries[i][0] == entries[i - 1][0]:
            sys.exit("duplicate key %s" % (entries[i][0],))

    base = HEADER_SIZE + ENTRY_SIZE * len(entries)
    index = bytearray()
    blobs = bytearray()

    for (shader_hash, stage, alpha), blob in entries:
        index += struct.pack("<IBBHII", shader_hash, stage, alpha, 0,
                             base + len(blobs), len(blob))
        blobs += blob
        while len(blobs) % 4:            # keep every blob 4-byte aligned
            blobs += b"\0"

    with open(args.output, "wb") as f:
        f.write(struct.pack("<4sIII", MAGIC, VERSION, len(entries), base))
        f.write(index)
        f.write(blobs)

    vertex = sum(1 for (_, stage, _), _ in entries if stage == STAGE_VERTEX)
    print("%d blobs packed (%d vertex, %d fragment), %.2f MB"
          % (len(entries), vertex, len(entries) - vertex,
             (base + len(blobs)) / 1048576.0))
    print("wrote %s" % args.output)


if __name__ == "__main__":
    main()
