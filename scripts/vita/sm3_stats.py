#!/usr/bin/env python3
"""Census of the dumped SM3 corpus: which opcodes, registers and features it actually uses.

  python sm3_stats.py <corpusdir>
"""

import os
import struct
import sys
from collections import Counter

OPCODES = {
    0: "nop", 1: "mov", 2: "add", 3: "sub", 4: "mad", 5: "mul", 6: "rcp", 7: "rsq",
    8: "dp3", 9: "dp4", 10: "min", 11: "max", 12: "slt", 13: "sge", 14: "exp", 15: "log",
    16: "lit", 17: "dst", 18: "lrp", 19: "frc", 20: "m4x4", 21: "m4x3", 22: "m3x4",
    23: "m3x3", 24: "m3x2", 25: "call", 26: "callnz", 27: "loop", 28: "ret", 29: "endloop",
    30: "label", 31: "dcl", 32: "pow", 33: "crs", 34: "sgn", 35: "abs", 36: "nrm",
    37: "sincos", 38: "rep", 39: "endrep", 40: "if", 41: "ifc", 42: "else", 43: "endif",
    44: "break", 45: "breakc", 46: "mova", 47: "defb", 48: "defi",
    64: "texcoord", 65: "texkill", 66: "texld", 67: "texbem", 68: "texbeml", 69: "texreg2ar",
    70: "texreg2gb", 71: "texm3x2pad", 72: "texm3x2tex", 73: "texm3x3pad", 74: "texm3x3tex",
    76: "texm3x3spec", 77: "texm3x3vspec", 78: "expp", 79: "logp", 80: "cnd", 81: "def",
    82: "texreg2rgb", 83: "texdp3tex", 84: "texm3x2depth", 85: "texdp3", 86: "texm3x3",
    87: "texdepth", 88: "cmp", 89: "bem", 90: "dp2add", 91: "dsx", 92: "dsy", 93: "texldd",
    94: "setp", 95: "texldl", 96: "breakp",
    0xFFFD: "phase",
}

REGTYPES = {
    0: "temp", 1: "input", 2: "const", 3: "addr/texture", 4: "rastout", 5: "attrout",
    6: "output", 7: "constint", 8: "colorout", 9: "depthout", 10: "sampler", 11: "const2",
    12: "const3", 13: "const4", 14: "constbool", 15: "loop", 16: "tempfloat16",
    17: "misctype", 18: "label", 19: "predicate",
}

# opcodes whose trailing tokens are literals, not parameter tokens: dst + N literals
LITERAL_OPS = {81: 4, 48: 4, 47: 1}

FLOW_OPS = {25, 26, 27, 28, 29, 30, 38, 39, 40, 41, 42, 43, 44, 45, 96}

# ops whose first trailing token is a source, not a destination
NO_DST_OPS = {0, 25, 26, 27, 28, 29, 30, 38, 39, 40, 41, 42, 43, 44, 45, 96}

SRC_MODIFIERS = {
    0: "none", 1: "neg", 2: "bias", 3: "bias_neg", 4: "sign", 5: "sign_neg", 6: "comp",
    7: "x2", 8: "x2_neg", 9: "dz", 10: "dw", 11: "abs", 12: "abs_neg", 13: "not",
}

DCL_USAGE = {
    0: "position", 1: "blendweight", 2: "blendindices", 3: "normal", 4: "psize",
    5: "texcoord", 6: "tangent", 7: "binormal", 8: "tessfactor", 9: "positiont",
    10: "color", 11: "fog", 12: "depth", 13: "sample",
}

TEXTURE_TYPES = {1: "1d", 2: "2d", 3: "cube", 4: "volume"}


def regtype(tok):
    return ((tok >> 28) & 0x7) | ((tok >> 8) & 0x18)


def regnum(tok):
    return tok & 0x7FF


class Shader(object):
    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = f.read()
        self.name = os.path.basename(path)
        self.is_vs = self.name.endswith(".vs")
        self.opcodes = Counter()
        self.maxreg = {}
        self.regs_used = set()
        self.predicated = 0
        self.relative = 0
        self.instructions = 0
        self.src_mods = Counter()
        self.dst_mods = Counter()
        self.dcls = Counter()
        self.parse()

    def note_reg(self, tok):
        t, n = regtype(tok), regnum(tok)
        self.regs_used.add((t, n))
        if n > self.maxreg.get(t, -1):
            self.maxreg[t] = n
        if (tok >> 13) & 0x3:
            self.relative += 1

    def parse(self):
        data = self.data
        p = 4
        while p + 4 <= len(data):
            tok = struct.unpack_from("<I", data, p)[0]
            if tok == 0x0000FFFF:
                return
            opcode = tok & 0xFFFF
            if opcode == 0xFFFE:
                p += 4 * (1 + ((tok >> 16) & 0x7FFF))
                continue

            length = (tok >> 24) & 0xF
            self.opcodes[opcode] += 1
            self.instructions += 1
            if (tok >> 28) & 1:
                self.predicated += 1

            body = p + 4
            if opcode == 31:  # dcl: usage token then the declared register
                if length >= 2:
                    usage = struct.unpack_from("<I", data, body)[0]
                    reg = struct.unpack_from("<I", data, body + 4)[0]
                    self.note_reg(reg)
                    if regtype(reg) == 10:
                        self.dcls["sampler %s" % TEXTURE_TYPES.get((usage >> 27) & 0x7, "?")] += 1
                    else:
                        self.dcls["%s %s%d" % (REGTYPES.get(regtype(reg), "?"),
                                               DCL_USAGE.get(usage & 0x1F, "?"),
                                               (usage >> 16) & 0xF)] += 1
            elif opcode in LITERAL_OPS:
                self.note_reg(struct.unpack_from("<I", data, body)[0])
            else:
                for i in range(length):
                    tok_p = struct.unpack_from("<I", data, body + i * 4)[0]
                    self.note_reg(tok_p)
                    if i == 0 and opcode not in NO_DST_OPS:
                        if (tok_p >> 20) & 0x1:
                            self.dst_mods["saturate"] += 1
                        shift = (tok_p >> 24) & 0xF
                        if shift:
                            self.dst_mods["shift %d" % (shift if shift < 8 else shift - 16)] += 1
                    else:
                        self.src_mods[SRC_MODIFIERS.get((tok_p >> 24) & 0xF, "?")] += 1

            p += 4 * (1 + length)


def main():
    corpus = sys.argv[1]
    shaders = [Shader(os.path.join(corpus, f)) for f in sorted(os.listdir(corpus))
               if f.endswith((".vs", ".ps"))]
    vs = [s for s in shaders if s.is_vs]
    ps = [s for s in shaders if not s.is_vs]

    print("=== %d shaders (%d vs, %d ps) ===\n" % (len(shaders), len(vs), len(ps)))

    total = Counter()
    shader_count = Counter()
    for s in shaders:
        total.update(s.opcodes)
        for op in s.opcodes:
            shader_count[op] += 1

    print("%-14s %8s %8s" % ("opcode", "uses", "shaders"))
    for op, n in total.most_common():
        print("%-14s %8d %8d" % (OPCODES.get(op, "UNKNOWN_%d" % op), n, shader_count[op]))

    print("\n=== register high-water marks ===")
    for label, group in (("vs", vs), ("ps", ps)):
        if not group:
            continue
        agg = {}
        for s in group:
            for t, n in s.maxreg.items():
                agg[t] = max(agg.get(t, -1), n)
        print("  %s:" % label)
        for t in sorted(agg):
            count = max(len(set(n for tt, n in s.regs_used if tt == t)) for s in group)
            print("    %-14s max index %3d, most used by one shader %3d"
                  % (REGTYPES.get(t, "type%d" % t), agg[t], count))

    print("\n=== instruction counts ===")
    for label, group in (("vs", vs), ("ps", ps)):
        if not group:
            continue
        counts = sorted(s.instructions for s in group)
        print("  %s: min %d, median %d, max %d"
              % (label, counts[0], counts[len(counts) // 2], counts[-1]))

    print("\n=== features ===")
    print("  shaders using flow control: %d"
          % sum(1 for s in shaders if any(op in FLOW_OPS for op in s.opcodes)))
    print("  shaders using predication:  %d" % sum(1 for s in shaders if s.predicated))
    print("  shaders using relative addressing: %d" % sum(1 for s in shaders if s.relative))
    unknown = [op for op in total if op not in OPCODES]
    print("  unknown opcodes: %s" % (sorted(unknown) if unknown else "none"))

    src_mods, dst_mods, dcls = Counter(), Counter(), Counter()
    for s in shaders:
        src_mods.update(s.src_mods)
        dst_mods.update(s.dst_mods)
        dcls.update(s.dcls)

    print("\n=== source modifiers ===")
    for mod, n in src_mods.most_common():
        print("  %-12s %d" % (mod, n))
    print("\n=== destination modifiers ===")
    for mod, n in dst_mods.most_common() or [("none", 0)]:
        print("  %-12s %d" % (mod, n))
    print("\n=== declarations ===")
    for d, n in dcls.most_common(20):
        print("  %-24s %d" % (d, n))


if __name__ == "__main__":
    main()
