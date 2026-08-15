#!/usr/bin/env python3
"""Translate CoD4's D3D9 SM3 shader bytecode to Cg for psp2cgc.

Covers the instruction set the shipped corpus actually uses, as measured by
sm3_stats.py: 33 opcodes, source modifiers neg/abs/abs_neg, saturate, structured
if/else, and 2d/3d/cube samplers. Anything outside that raises, so an unhandled
construct is a hard failure rather than a silently wrong shader.

  python sm3_to_cg.py <in.vs|in.ps> [out.cg]
"""

import os
import struct
import sys

TEMP, INPUT, CONST, ADDR, RASTOUT, ATTROUT, OUTPUT = 0, 1, 2, 3, 4, 5, 6
CONSTINT, COLOROUT, DEPTHOUT, SAMPLER = 7, 8, 9, 10

END_TOKEN = 0x0000FFFF
COMMENT = 0xFFFE

DCL_USAGE = {
    0: "POSITION", 3: "NORMAL", 5: "TEXCOORD", 6: "TANGENT", 7: "BINORMAL",
    10: "COLOR", 11: "FOG", 12: "DEPTH", 1: "BLENDWEIGHT", 2: "BLENDINDICES",
    4: "PSIZE",
}

SAMPLER_TYPES = {2: ("sampler2D", "tex2D"), 3: ("samplerCUBE", "texCUBE"),
                 4: ("sampler2D", "sampleVolume")}

VOLUME = 4

# GXM has no volume texture type (GPU guide, table 4), so a 3D lookup samples two
# slices of a strip atlas the loader builds. The atlas carries no mips.
VOLUME_HELPER = """float4 sampleVolume(sampler2D tex, float3 uvw, float2 layout)
{
\tfloat slice = uvw.z * layout.x - 0.5;
\tfloat base = floor(slice);
\tfloat v0 = (clamp(base, 0.0, layout.x - 1.0) + uvw.y) * layout.y;
\tfloat v1 = (clamp(base + 1.0, 0.0, layout.x - 1.0) + uvw.y) * layout.y;
\t// the taps are half4 in this profile; widening them keeps the lerp overload unambiguous
\tfloat4 a = tex2D(tex, float2(uvw.x, v0));
\tfloat4 b = tex2D(tex, float2(uvw.x, v1));
\treturn lerp(a, b, slice - base);
}
"""


class Unsupported(Exception):
    pass


def swizzle_str(bits, count=4):
    comps = "xyzw"
    s = "".join(comps[(bits >> (i * 2)) & 3] for i in range(count))
    return "" if s == "xyzw" else "." + s


def mask_str(bits):
    if bits == 0xF:
        return ""
    return "." + "".join(c for i, c in enumerate("xyzw") if bits & (1 << i))


class Src(object):
    def __init__(self, tok):
        self.num = tok & 0x7FF
        self.type = ((tok >> 28) & 0x7) | ((tok >> 8) & 0x18)
        self.swizzle = (tok >> 16) & 0xFF
        self.mod = (tok >> 24) & 0xF
        if (tok >> 13) & 0x3:
            raise Unsupported("relative addressing")


class Dst(object):
    def __init__(self, tok):
        self.num = tok & 0x7FF
        self.type = ((tok >> 28) & 0x7) | ((tok >> 8) & 0x18)
        self.mask = (tok >> 16) & 0xF
        self.saturate = bool((tok >> 20) & 0x1)
        if (tok >> 24) & 0xF:
            raise Unsupported("destination shift scale")


class Instruction(object):
    def __init__(self, opcode, dst, srcs, control):
        self.opcode = opcode
        self.dst = dst
        self.srcs = srcs
        self.control = control


NO_DST_OPS = {0, 25, 26, 27, 28, 29, 30, 38, 39, 40, 41, 42, 43, 44, 45, 96}
LITERAL_OPS = {81: 4, 48: 4, 47: 1}


class Shader(object):
    def __init__(self, data):
        self.data = data
        version = struct.unpack_from("<I", data, 0)[0]
        self.is_vs = (version >> 16) == 0xFFFE
        self.instructions = []
        self.defs = {}          # const register -> 4 floats declared inline
        self.int_defs = {}      # integer constant register -> 4 ints, loop counts
        self.decls = {}         # (regtype, num) -> (usage, index)
        self.samplers = {}      # num -> texture type
        self.temps = set()
        self.consts = set()
        self.parse()

    def parse(self):
        data = self.data
        p = 4
        while p + 4 <= len(data):
            tok = struct.unpack_from("<I", data, p)[0]
            if tok == END_TOKEN:
                return
            opcode = tok & 0xFFFF
            if opcode == COMMENT:
                p += 4 * (1 + ((tok >> 16) & 0x7FFF))
                continue
            if (tok >> 28) & 1:
                raise Unsupported("predicated instruction")

            length = (tok >> 24) & 0xF
            control = (tok >> 16) & 0xFF
            body = p + 4

            if opcode == 31:    # dcl
                usage = struct.unpack_from("<I", data, body)[0]
                dst = Dst(struct.unpack_from("<I", data, body + 4)[0])
                if dst.type == SAMPLER:
                    self.samplers[dst.num] = (usage >> 27) & 0x7
                else:
                    self.decls[(dst.type, dst.num)] = (usage & 0x1F, (usage >> 16) & 0xF)
                self.note_reg(dst.type, dst.num)
            elif opcode in LITERAL_OPS:
                dst = Dst(struct.unpack_from("<I", data, body)[0])
                if opcode == 81:
                    self.defs[dst.num] = struct.unpack_from("<4f", data, body + 4)
                elif opcode == 48:
                    self.int_defs[dst.num] = struct.unpack_from("<4i", data, body + 4)
                else:
                    raise Unsupported("boolean constant declaration")
            else:
                toks = [struct.unpack_from("<I", data, body + i * 4)[0] for i in range(length)]
                dst = None
                if opcode not in NO_DST_OPS and toks:
                    dst = Dst(toks[0])
                    self.note_reg(dst.type, dst.num)
                    toks = toks[1:]
                srcs = [Src(t) for t in toks]
                for s in srcs:
                    self.note_reg(s.type, s.num)
                self.instructions.append(Instruction(opcode, dst, srcs, control))

            p += 4 * (1 + length)

    def note_reg(self, rtype, num):
        if rtype == TEMP:
            self.temps.add(num)
        elif rtype == CONST:
            self.consts.add(num)


class Emitter(object):
    def __init__(self, shader):
        self.sh = shader
        self.lines = []
        self.indent = 1
        self.rep_depth = 0

    def reg(self, rtype, num):
        if rtype == TEMP:
            return "r%d" % num
        if rtype == CONST:
            return "cd%d" % num if num in self.sh.defs else "c[%d]" % num
        if rtype == INPUT:
            return "v%d" % num
        if rtype == OUTPUT or rtype == ATTROUT:
            return "o%d" % num
        if rtype == RASTOUT:
            return "o_pos"
        if rtype == COLOROUT:
            return "oC%d" % num
        if rtype == SAMPLER:
            return "s%d" % num
        if rtype == CONSTINT:
            return "i%d" % num
        raise Unsupported("register type %d" % rtype)

    def src(self, s, want=None):
        text = self.reg(s.type, s.num)
        sw = swizzle_str(s.swizzle)
        if want is not None:
            # collapse the swizzle to the components this operand actually reads
            comps = "".join("xyzw"[(s.swizzle >> (i * 2)) & 3] for i in range(want))
            text += "." + comps
        elif sw:
            text += sw
        if s.mod == 1:
            text = "-(%s)" % text
        elif s.mod == 11:
            text = "abs(%s)" % text
        elif s.mod == 12:
            text = "-abs(%s)" % text
        elif s.mod != 0:
            raise Unsupported("source modifier %d" % s.mod)
        return text

    def emit(self, text):
        self.lines.append("\t" * self.indent + text)

    def assign(self, dst, expr, scalar=False):
        target = self.reg(dst.type, dst.num) + mask_str(dst.mask)
        if dst.saturate:
            expr = "saturate(%s)" % expr
        if not scalar and mask_str(dst.mask):
            expr = "(%s)%s" % (expr, mask_str(dst.mask))
        self.emit("%s = %s;" % (target, expr))

    def matrix(self, ins, rows, size):
        # m4x4 and friends: consecutive constant rows dotted against one source
        base = ins.srcs[1]
        parts = []
        for i in range(rows):
            row = Src.__new__(Src)
            row.num = base.num + i
            row.type = base.type
            row.swizzle = base.swizzle
            row.mod = base.mod
            parts.append("dot(%s, %s)" % (self.src(ins.srcs[0], size), self.src(row, size)))
        return "float%d(%s)" % (rows, ", ".join(parts))

    def instruction(self, ins):
        op, dst, srcs = ins.opcode, ins.dst, ins.srcs
        s = [self.src(x) for x in srcs]

        if op == 1:                                     # mov
            self.assign(dst, s[0])
        elif op == 2:
            self.assign(dst, "%s + %s" % (s[0], s[1]))
        elif op == 3:
            self.assign(dst, "%s - %s" % (s[0], s[1]))
        elif op == 4:
            self.assign(dst, "%s * %s + %s" % (s[0], s[1], s[2]))
        elif op == 5:
            self.assign(dst, "%s * %s" % (s[0], s[1]))
        elif op == 6:                                   # rcp
            self.assign(dst, "1.0 / (%s)" % self.src(srcs[0], 1), scalar=True)
        elif op == 7:                                   # rsq, defined on |src|
            self.assign(dst, "rsqrt(abs(%s))" % self.src(srcs[0], 1), scalar=True)
        elif op == 8:
            self.assign(dst, "dot(%s, %s)" % (self.src(srcs[0], 3), self.src(srcs[1], 3)),
                        scalar=True)
        elif op == 9:
            self.assign(dst, "dot(%s, %s)" % (self.src(srcs[0], 4), self.src(srcs[1], 4)),
                        scalar=True)
        elif op == 10:
            self.assign(dst, "min(%s, %s)" % (s[0], s[1]))
        elif op == 11:
            self.assign(dst, "max(%s, %s)" % (s[0], s[1]))
        elif op == 12:
            self.assign(dst, "(%s < %s) ? 1.0 : 0.0" % (s[0], s[1]))
        elif op == 13:
            self.assign(dst, "(%s >= %s) ? 1.0 : 0.0" % (s[0], s[1]))
        elif op == 14 or op == 78:                      # exp / expp
            self.assign(dst, "exp2(%s)" % self.src(srcs[0], 1), scalar=True)
        elif op == 15 or op == 79:                      # log / logp
            self.assign(dst, "log2(abs(%s))" % self.src(srcs[0], 1), scalar=True)
        elif op == 18:                                  # lrp
            self.assign(dst, "lerp(%s, %s, %s)" % (s[2], s[1], s[0]))
        elif op == 19:
            self.assign(dst, "frac(%s)" % s[0])
        elif op in (20, 21, 22, 23, 24):
            rows, size = {20: (4, 4), 21: (3, 4), 22: (4, 3), 23: (3, 3), 24: (2, 3)}[op]
            self.assign(dst, self.matrix(ins, rows, size))
        elif op == 32:                                  # pow, defined on |src0|
            self.assign(dst, "pow(abs(%s), %s)" % (self.src(srcs[0], 1), self.src(srcs[1], 1)),
                        scalar=True)
        elif op == 33:
            self.assign(dst, "cross(%s, %s)" % (self.src(srcs[0], 3), self.src(srcs[1], 3)))
        elif op == 35:
            self.assign(dst, "abs(%s)" % s[0])
        elif op == 36:                                  # nrm, xyz only
            self.assign(dst, "normalize(%s)" % self.src(srcs[0], 3))
        elif op == 37:                                  # sincos: cos in x, sin in y
            arg = self.src(srcs[0], 1)
            self.assign(dst, "float4(cos(%s), sin(%s), 0.0, 0.0)" % (arg, arg))
        elif op == 88:                                  # cmp: per component src0 >= 0
            self.assign(dst, "(%s >= 0.0) ? %s : %s" % (s[0], s[1], s[2]))
        elif op == 80:                                  # cnd: src0 > 0.5
            self.assign(dst, "(%s > 0.5) ? %s : %s" % (s[0], s[1], s[2]))
        elif op == 90:                                  # dp2add
            self.assign(dst, "dot(%s, %s) + %s"
                        % (self.src(srcs[0], 2), self.src(srcs[1], 2), self.src(srcs[2], 1)),
                        scalar=True)
        elif op == 91:
            self.assign(dst, "ddx(%s)" % s[0])
        elif op == 92:
            self.assign(dst, "ddy(%s)" % s[0])
        elif op == 66:                                  # texld
            self.assign(dst, self.sample(srcs[1], srcs[0]))
        elif op == 95:                                  # texldl, lod in src0.w
            self.assign(dst, self.sample(srcs[1], srcs[0], lod=True))
        elif op == 93:                                  # texldd
            self.assign(dst, self.sample(srcs[1], srcs[0], grad=(srcs[2], srcs[3])))
        elif op == 65:                                  # texkill reads its operand from the dst slot
            self.emit("if (any(%s.xyz < 0.0)) discard;" % self.reg(dst.type, dst.num))
        elif op == 41:                                  # ifc
            cmp = {1: ">", 2: "==", 3: ">=", 4: "!=", 5: "<", 6: "<="}[ins.control]
            self.emit("if (%s %s %s) {" % (self.src(srcs[0], 1), cmp, self.src(srcs[1], 1)))
            self.indent += 1
        elif op == 40:                                  # if
            self.emit("if (%s != 0.0) {" % self.src(srcs[0], 1))
            self.indent += 1
        elif op == 42:
            self.indent -= 1
            self.emit("} else {")
            self.indent += 1
        elif op == 43 or op == 39:
            self.indent -= 1
            self.emit("}")
        elif op == 38:                                  # rep: fixed trip count, aL is unused here
            count = self.sh.int_defs.get(srcs[0].num, (1, 0, 0, 0))[0]
            self.emit("for (int rep%d = 0; rep%d < %d; ++rep%d) {"
                      % (self.rep_depth, self.rep_depth, count, self.rep_depth))
            self.rep_depth += 1
            self.indent += 1
        elif op == 0:
            pass
        else:
            raise Unsupported("opcode %d" % op)

    def sample(self, sampler, coord, lod=False, grad=None):
        stype = self.sh.samplers.get(sampler.num)
        if stype not in SAMPLER_TYPES:
            raise Unsupported("sampler type %s" % stype)
        _, fn = SAMPLER_TYPES[stype]
        dims = {2: 2, 3: 3, 4: 3}[stype]
        name = self.reg(SAMPLER, sampler.num)
        if stype == VOLUME:
            return "sampleVolume(%s, %s, volumeLayout_s%d.xy)" % (
                name, self.src(coord, 3), sampler.num)
        if lod:
            # the lod goes in .w, so a 2d lookup pads to four components and a 3d one does not
            lod_src = "%s.w" % self.reg(coord.type, coord.num)
            if dims == 2:
                return "%slod(%s, float4(%s, 0.0, %s))" % (fn, name, self.src(coord, 2), lod_src)
            return "%slod(%s, float4(%s, %s))" % (fn, name, self.src(coord, 3), lod_src)
        if grad:
            return "%s(%s, %s, %s, %s)" % (fn, name, self.src(coord, dims),
                                           self.src(grad[0], dims), self.src(grad[1], dims))
        return "%s(%s, %s)" % (fn, name, self.src(coord, dims))


def semantic(usage, index):
    name = DCL_USAGE.get(usage)
    if not name:
        raise Unsupported("declaration usage %d" % usage)
    return "%s%d" % (name, index)


def translate(data):
    sh = Shader(data)
    em = Emitter(sh)

    params = []
    for (rtype, num), (usage, index) in sorted(sh.decls.items()):
        if rtype == INPUT:
            params.append("float4 v%d : %s" % (num, semantic(usage, index)))
        elif rtype in (OUTPUT, ATTROUT) and sh.is_vs:
            params.append("out float4 o%d : %s" % (num, semantic(usage, index)))

    uniform_consts = sorted(c for c in sh.consts if c not in sh.defs)
    if uniform_consts:
        params.append("uniform float4 c[%d]" % (max(uniform_consts) + 1))
    for num in sorted(sh.samplers):
        stype = sh.samplers[num]
        if stype not in SAMPLER_TYPES:
            raise Unsupported("sampler type %s" % stype)
        params.append("uniform %s s%d : TEXUNIT%d" % (SAMPLER_TYPES[stype][0], num, num))
        if stype == VOLUME:
            params.append("uniform float4 volumeLayout_s%d" % num)

    body = []
    if sh.temps:
        body.append("\tfloat4 %s;" % ", ".join("r%d" % n for n in sorted(sh.temps)))
    for num in sorted(sh.defs):
        body.append("\tconst float4 cd%d = float4(%s);"
                    % (num, ", ".join("%g" % v for v in sh.defs[num])))

    if not sh.is_vs:
        body.append("\tfloat4 oC0;")

    for ins in sh.instructions:
        em.instruction(ins)
    body.extend(em.lines)

    if sh.is_vs:
        signature = "void main(\n\t%s)" % ",\n\t".join(params)
    else:
        body.append("\treturn oC0;")
        signature = "float4 main(\n\t%s) : COLOR" % ",\n\t".join(params)

    text = "// generated from SM3 bytecode by sm3_to_cg.py -- do not edit\n\n"
    if any(t == VOLUME for t in sh.samplers.values()):
        text += VOLUME_HELPER + "\n"
    text += signature + "\n{\n" + "\n".join(body) + "\n}\n"
    return text, sh


def main():
    src = sys.argv[1]
    with open(src, "rb") as f:
        data = f.read()
    text, _ = translate(data)
    if len(sys.argv) > 2:
        with open(sys.argv[2], "w", newline="\n") as f:
            f.write(text)
    else:
        sys.stdout.write(text)


if __name__ == "__main__":
    main()
