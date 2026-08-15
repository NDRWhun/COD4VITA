#!/usr/bin/env python3
"""Read the D3DX constant table embedded in SM3 bytecode.

Every CoD4 shader carries one, so constants and samplers can be bound by name
instead of by raw register index.
"""

import struct

REGISTER_SETS = {0: "bool", 1: "int4", 2: "float4", 3: "sampler"}


class Constant(object):
    def __init__(self, name, regset, index, count):
        self.name = name
        self.regset = regset
        self.index = index
        self.count = count

    def __repr__(self):
        return "%s %s c%d[%d]" % (self.name, self.regset, self.index, self.count)


def _cstring(data, off):
    end = data.index(b"\0", off)
    return data[off:end].decode("ascii", "replace")


def parse(data):
    """Constants declared by the shader, or [] if it carries no table."""
    magic = data.find(b"CTAB")
    if magic < 0:
        return []

    base = magic + 4
    size, creator, version, count, info_off, flags, target = struct.unpack_from("<7I", data, base)
    (creator, version, flags, target)  # present in the table, unused here

    out = []
    for i in range(count):
        off = base + info_off + i * 20
        name_off, regset, index, reg_count, _reserved, _type, _default = \
            struct.unpack_from("<I4HII", data, off)
        out.append(Constant(_cstring(data, base + name_off),
                            REGISTER_SETS.get(regset, "set%d" % regset),
                            index, reg_count))
    return out


if __name__ == "__main__":
    import sys

    with open(sys.argv[1], "rb") as f:
        for c in parse(f.read()):
            print(c)
