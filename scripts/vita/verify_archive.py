#!/usr/bin/env python3
"""Check the shader archive against the corpus the runtime will look up in it.

The runtime hashes the SM3 bytecode the engine hands it and binary-searches the
index. This transcribes those rules -- token stream length, FNV-1a, key order --
and checks that every shader resolves, so a mismatch surfaces here instead of as
an empty screen on hardware.

  python verify_archive.py <corpusdir> <archive.kgxp>
"""

import os
import struct
import sys

HEADER_SIZE = 16
ENTRY_SIZE = 16
STAGE_VERTEX, STAGE_FRAGMENT = 0, 1


def bytecode_length(data):
    """Mirrors GxmShaderArchive_BytecodeLength: version token through END, in bytes."""
    limit = len(data) // 4
    i = 1
    while i < limit:
        token = struct.unpack_from("<I", data, i * 4)[0]
        if token == 0x0000FFFF:
            return (i + 1) * 4
        if (token & 0xFFFF) == 0xFFFE:
            i += 1 + ((token >> 16) & 0x7FFF)
        else:
            i += 1 + ((token >> 24) & 0xF)
    return 0


def fnv1a(data):
    """Mirrors GxmShaderArchive_HashBytecode."""
    h = 2166136261
    for b in data:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def main():
    corpus, path = sys.argv[1], sys.argv[2]

    with open(path, "rb") as f:
        blob = f.read()

    magic, version, count, base = struct.unpack_from("<4sIII", blob, 0)
    if magic != b"KGXP":
        sys.exit("not a shader archive")
    print("archive: version %d, %d entries, %.2f MB" % (version, count, len(blob) / 1048576.0))

    entries = []
    for i in range(count):
        h, stage, alpha, _res, offset, size = struct.unpack_from(
            "<IBBHII", blob, HEADER_SIZE + i * ENTRY_SIZE)
        entries.append((h, stage, alpha, offset, size))

    problems = []

    # the runtime binary-searches, so the index must be sorted on the same key
    keys = [(h, s, a) for h, s, a, _o, _n in entries]
    if keys != sorted(keys):
        problems.append("index is not sorted by (hash, stage, alphaTest)")

    for h, stage, alpha, offset, size in entries:
        if offset + size > len(blob):
            problems.append("entry %08x runs past the end of the archive" % h)
        elif blob[offset:offset + 4] != b"GXP\0":
            problems.append("entry %08x does not point at a GXP blob" % h)

    index = {(h, s, a) for h, s, a, _o, _n in entries}

    # every shader the engine can hand us must resolve, in every variant it may need
    checked = 0
    missing = []
    for name in sorted(os.listdir(corpus)):
        if not name.endswith((".vs", ".ps")):
            continue
        with open(os.path.join(corpus, name), "rb") as f:
            data = f.read()

        length = bytecode_length(data)
        if length != len(data):
            problems.append("%s: length rule gives %d, file is %d" % (name, length, len(data)))
            continue

        h = fnv1a(data[:length])
        if h != int(name.split(".")[0], 16):
            problems.append("%s: runtime hash %08x does not match the baked name" % (name, h))
            continue

        if name.endswith(".vs"):
            wanted = [(h, STAGE_VERTEX, 0)]
        else:
            wanted = [(h, STAGE_FRAGMENT, a) for a in range(4)]

        for key in wanted:
            checked += 1
            if key not in index:
                missing.append(key)

    print("resolved %d/%d expected lookups" % (checked - len(missing), checked))

    if missing:
        print("missing entries: %d" % len(missing))
        for key in missing[:5]:
            print("  hash %08x stage %d alphaTest %d" % key)
    if problems:
        print("problems: %d" % len(problems))
        for p in problems[:10]:
            print("  %s" % p)
    if not missing and not problems:
        print("archive is consistent with the corpus")


if __name__ == "__main__":
    main()
