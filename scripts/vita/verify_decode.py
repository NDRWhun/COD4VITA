#!/usr/bin/env python3
"""Check the SM3 decoder against D3DX's own disassembler.

Renders every instruction the way D3DXDisassembleShader prints it and diffs the
two streams, so a decode bug (wrong swizzle, mask, modifier or register type)
shows up as a mismatch rather than as wrong pixels later.

  python verify_decode.py <corpusdir> [--dll <path to d3dx9_*.dll>]
"""

import argparse
import ctypes
import os
import re
import sys
from ctypes import wintypes

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from sm3_to_cg import (ADDR, ATTROUT, COLOROUT, CONST, CONSTINT, DEPTHOUT, INPUT,
                       OUTPUT, RASTOUT, SAMPLER, TEMP, Shader, Unsupported)

OPNAMES = {
    0: "nop", 1: "mov", 2: "add", 3: "sub", 4: "mad", 5: "mul", 6: "rcp", 7: "rsq",
    8: "dp3", 9: "dp4", 10: "min", 11: "max", 12: "slt", 13: "sge", 14: "exp", 15: "log",
    16: "lit", 17: "dst", 18: "lrp", 19: "frc", 20: "m4x4", 21: "m4x3", 22: "m3x4",
    23: "m3x3", 24: "m3x2", 25: "call", 26: "callnz", 27: "loop", 28: "ret", 29: "endloop",
    30: "label", 32: "pow", 33: "crs", 34: "sgn", 35: "abs", 36: "nrm", 37: "sincos",
    38: "rep", 39: "endrep", 40: "if", 42: "else", 43: "endif", 44: "break", 46: "mova",
    64: "texcoord", 65: "texkill", 66: "texld", 78: "expp", 79: "logp", 80: "cnd",
    82: "texreg2rgb", 85: "texdp3", 86: "texm3x3", 87: "texdepth", 88: "cmp", 90: "dp2add",
    91: "dsx", 92: "dsy", 93: "texldd", 94: "setp", 95: "texldl", 96: "breakp",
}

# ifc/breakc encode the comparison in the instruction's control field
COMPARISONS = {1: "gt", 2: "eq", 3: "ge", 4: "lt", 5: "ne", 6: "le"}

REGPREFIX = {TEMP: "r", INPUT: "v", CONST: "c", ADDR: "a", RASTOUT: "oPos", ATTROUT: "oD",
             OUTPUT: "o", CONSTINT: "i", COLOROUT: "oC", DEPTHOUT: "oDepth", SAMPLER: "s"}


def load_disassembler(path):
    lib = ctypes.windll.LoadLibrary(path)
    fn = lib.D3DXDisassembleShader
    fn.restype = ctypes.c_long
    fn.argtypes = [ctypes.c_void_p, wintypes.BOOL, ctypes.c_char_p,
                   ctypes.POINTER(ctypes.c_void_p)]

    def disassemble(data):
        buf = ctypes.c_void_p()
        hr = fn(ctypes.create_string_buffer(data, len(data)), 0, None, ctypes.byref(buf))
        if hr < 0 or not buf:
            raise RuntimeError("D3DXDisassembleShader failed: 0x%08x" % (hr & 0xFFFFFFFF))
        vt = ctypes.cast(buf, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
        get_ptr = ctypes.CFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)(vt[3])
        get_size = ctypes.CFUNCTYPE(ctypes.c_ulong, ctypes.c_void_p)(vt[4])
        release = ctypes.CFUNCTYPE(ctypes.c_ulong, ctypes.c_void_p)(vt[2])
        text = ctypes.string_at(get_ptr(buf), get_size(buf)).decode("ascii", "replace")
        release(buf)
        return text

    return disassemble


def collapse(sw):
    """D3DX prints a uniform replicate swizzle as one component."""
    if len(sw) == 4 and len(set(sw)) == 1:
        return sw[0]
    return sw


def render_mask(mask):
    if mask == 0xF:
        return ""
    return "." + "".join(c for i, c in enumerate("xyzw") if mask & (1 << i))


def render_swizzle(bits):
    sw = "".join("xyzw"[(bits >> (i * 2)) & 3] for i in range(4))
    if sw == "xyzw":
        return ""
    return "." + collapse(sw)


def render_reg(rtype, num):
    prefix = REGPREFIX.get(rtype)
    if prefix is None:
        raise Unsupported("register type %d" % rtype)
    if rtype in (RASTOUT, DEPTHOUT):
        return prefix
    return "%s%d" % (prefix, num)


def render_dst(dst):
    return render_reg(dst.type, dst.num) + render_mask(dst.mask)


def render_src(src):
    reg = render_reg(src.type, src.num)
    if src.mod in (11, 12):
        reg += "_abs"
    text = reg + render_swizzle(src.swizzle)
    if src.mod in (1, 12):
        return "-" + text
    return text


def render_instruction(ins):
    if ins.opcode == 41:
        name = "if_" + COMPARISONS.get(ins.control, "?")
    elif ins.opcode == 45:
        name = "break_" + COMPARISONS.get(ins.control, "?")
    elif ins.opcode == 66:
        # the control field selects plain, projective or biased sampling
        name = {0: "texld", 1: "texldp", 2: "texldb"}.get(ins.control & 0x3, "texld")
    else:
        name = OPNAMES.get(ins.opcode)
        if name is None:
            raise Unsupported("opcode %d" % ins.opcode)

    ops = []
    if ins.dst is not None:
        if ins.dst.saturate:
            name += "_sat"
        ops.append(render_dst(ins.dst))
    ops.extend(render_src(s) for s in ins.srcs)
    return "%s %s" % (name, ", ".join(ops)) if ops else name


def normalize(line):
    line = line.replace("\0", "").strip()
    line = re.sub(r"_pp\b", "", line)           # partial precision is a hint, not semantics
    line = re.sub(r"_centroid\b", "", line)
    line = re.sub(r"\s+", " ", line)
    return line


def reference_instructions(text):
    out = []
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("//"):
            continue
        if re.match(r"^(vs|ps)_\d_\d$", line):
            continue
        if line.startswith("dcl") or line.startswith("def"):
            continue
        text = normalize(line)
        if text:                    # the buffer ends with a NUL that would read as a line
            out.append(text)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("corpus")
    ap.add_argument("--dll", default=r"C:\Windows\System32\d3dx9_34.dll")
    ap.add_argument("--show", type=int, default=6, help="mismatching shaders to print")
    args = ap.parse_args()

    disassemble = load_disassembler(args.dll)
    names = sorted(f for f in os.listdir(args.corpus) if f.endswith((".vs", ".ps")))

    checked = matched = 0
    instructions = 0
    shown = 0
    failed = []

    for name in names:
        with open(os.path.join(args.corpus, name), "rb") as f:
            data = f.read()

        reference = reference_instructions(disassemble(data))
        try:
            mine = [normalize(render_instruction(i)) for i in Shader(data).instructions]
        except Unsupported as e:
            failed.append((name, "render: %s" % e))
            continue

        checked += 1
        if mine == reference:
            matched += 1
            instructions += len(mine)
            continue

        diffs = [(a, b) for a, b in zip(reference, mine) if a != b]
        failed.append((name, "%d/%d instructions differ" % (len(diffs), len(reference))))
        if shown < args.show:
            shown += 1
            print("--- %s (%d ref, %d mine) ---" % (name, len(reference), len(mine)))
            for a, b in diffs[:4]:
                print("   d3dx: %s\n   mine: %s" % (a, b))

    print("\n=== %d/%d shaders match D3DX exactly (%d instructions verified) ==="
          % (matched, checked, instructions))
    if failed:
        print("%d mismatched:" % len(failed))
        for name, why in failed[:20]:
            print("  %-16s %s" % (name, why))


if __name__ == "__main__":
    main()
