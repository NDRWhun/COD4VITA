#!/usr/bin/env python3
"""Differential test: execute the SM3 bytecode and the generated Cg, compare results.

verify_decode.py already checks the decoder against D3DX, so what is under test
here is the Cg emission -- write masks, swizzles, source modifiers, saturate,
argument order and scalar replication. Both sides run identical random inputs
through identical stub samplers, so a texture read checks which coordinates were
passed rather than any filtering behaviour.

  python verify_semantics.py <corpusdir> [--trials 3]
"""

import argparse
import math
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import cg_eval
from cg_eval import Vec
from sm3_to_cg import (ATTROUT, COLOROUT, CONST, CONSTINT, INPUT, OUTPUT, RASTOUT,
                       SAMPLER, TEMP, Shader, Unsupported, translate)


def make_stub(shader):
    """Deterministic stand-in for a texture fetch, shared by both sides.

    A 2D sampler ignores the third coordinate in hardware, so the stub drops it
    too -- otherwise the two sides diverge on what the lookup even reads.
    """

    def sample(unit, coord):
        c = list(coord.v) if isinstance(coord, Vec) else (list(coord) + [0.0] * 4)[:4]
        if shader.samplers.get(unit) == 2:
            c[2] = 0.0
        base = math.sin(unit * 1.7 + c[0] * 2.3 + c[1] * 3.1 + c[2] * 0.7)
        return [0.5 + 0.5 * base,
                0.5 + 0.5 * math.sin(base * 2.0 + unit),
                0.5 + 0.5 * math.cos(base * 1.3),
                0.5 + 0.25 * base]

    return sample


class Discarded(Exception):
    pass


class Interpreter(object):
    """Executes the decoded stream directly, as the D3D9 instruction set defines it."""

    def __init__(self, shader, inputs, constants, sampler):
        self.sh = shader
        self.r = {}
        self.v = inputs
        self.c = constants
        self.sample = sampler
        self.o = {}
        self.oC = {}

    def read(self, src):
        if src.type == TEMP:
            val = self.r.get(src.num, [0.0] * 4)
        elif src.type == INPUT:
            val = self.v.get(src.num, [0.0] * 4)
        elif src.type == CONST:
            val = list(self.sh.defs[src.num]) if src.num in self.sh.defs \
                else self.c.get(src.num, [0.0] * 4)
        elif src.type == CONSTINT:
            val = [float(x) for x in self.sh.int_defs.get(src.num, (0, 0, 0, 0))]
        elif src.type == SAMPLER:
            return [0.0] * 4          # sampler operands carry only their unit number
        else:
            raise Unsupported("read register type %d" % src.type)

        out = [val[(src.swizzle >> (i * 2)) & 3] for i in range(4)]
        if src.mod == 1:
            return [-x for x in out]
        if src.mod == 11:
            return [abs(x) for x in out]
        if src.mod == 12:
            return [-abs(x) for x in out]
        if src.mod:
            raise Unsupported("source modifier %d" % src.mod)
        return out

    def write(self, dst, value):
        if not isinstance(value, list):
            value = [float(value)] * 4
        value = (value + [value[-1]] * 4)[:4]
        if dst.saturate:
            value = [min(1.0, max(0.0, x)) for x in value]

        if dst.type == TEMP:
            target = self.r.setdefault(dst.num, [0.0] * 4)
        elif dst.type in (OUTPUT, ATTROUT):
            target = self.o.setdefault(dst.num, [0.0] * 4)
        elif dst.type == COLOROUT:
            target = self.oC.setdefault(dst.num, [0.0] * 4)
        elif dst.type == RASTOUT:
            target = self.o.setdefault(-1, [0.0] * 4)
        else:
            raise Unsupported("write register type %d" % dst.type)

        for i in range(4):
            if dst.mask & (1 << i):
                target[i] = value[i]

    def run(self):
        self.execute(self.sh.instructions, 0, len(self.sh.instructions))
        return self.o, self.oC

    def matching(self, ins, start, stop, wanted):
        depth = 0
        for i in range(start, stop):
            op = ins[i].opcode
            if op in (40, 41, 38):
                depth += 1
            elif op in (43, 39):
                depth -= 1
                if depth == 0:
                    return i
            elif op == 42 and depth == 1 and wanted == "else":
                return i
        return stop

    def execute(self, ins, start, stop):
        i = start
        while i < stop:
            op = ins[i].opcode

            if op in (40, 41):
                srcs = [self.read(s) for s in ins[i].srcs]
                if op == 40:
                    taken = srcs[0][0] != 0.0
                else:
                    a, b = srcs[0][0], srcs[1][0]
                    taken = {1: a > b, 2: a == b, 3: a >= b,
                             4: a < b, 5: a != b, 6: a <= b}[ins[i].control]

                els = self.matching(ins, i, stop, "else")
                end = self.matching(ins, i, stop, "endif")
                if els < end:
                    if taken:
                        self.execute(ins, i + 1, els)
                    else:
                        self.execute(ins, els + 1, end)
                elif taken:
                    self.execute(ins, i + 1, end)
                i = end + 1
                continue

            if op == 38:
                count = self.sh.int_defs.get(ins[i].srcs[0].num, (1, 0, 0, 0))[0]
                end = self.matching(ins, i, stop, "endif")
                for _ in range(count):
                    self.execute(ins, i + 1, end)
                i = end + 1
                continue

            if op in (42, 43, 39):
                i += 1
                continue

            self.step(ins[i])
            i += 1

    def step(self, ins):
        op, dst = ins.opcode, ins.dst
        s = [self.read(x) for x in ins.srcs]

        def each(fn):
            self.write(dst, [fn(*[x[i] for x in s]) for i in range(4)])

        if op == 1:
            self.write(dst, s[0])
        elif op == 2:
            each(lambda a, b: a + b)
        elif op == 3:
            each(lambda a, b: a - b)
        elif op == 4:
            each(lambda a, b, c: a * b + c)
        elif op == 5:
            each(lambda a, b: a * b)
        elif op == 6:
            self.write(dst, 1.0 / s[0][0] if s[0][0] else 0.0)
        elif op == 7:
            self.write(dst, 1.0 / math.sqrt(abs(s[0][0])) if s[0][0] else 0.0)
        elif op == 8:
            self.write(dst, sum(s[0][i] * s[1][i] for i in range(3)))
        elif op == 9:
            self.write(dst, sum(s[0][i] * s[1][i] for i in range(4)))
        elif op == 10:
            each(min)
        elif op == 11:
            each(max)
        elif op == 12:
            each(lambda a, b: 1.0 if a < b else 0.0)
        elif op == 13:
            each(lambda a, b: 1.0 if a >= b else 0.0)
        elif op in (14, 78):
            self.write(dst, 2.0 ** s[0][0] if s[0][0] < 128 else float("inf"))
        elif op in (15, 79):
            self.write(dst, math.log(abs(s[0][0]), 2) if s[0][0] else -1e30)
        elif op == 18:
            each(lambda a, b, c: c + a * (b - c))
        elif op == 19:
            each(lambda a: a - math.floor(a))
        elif op in (20, 21, 22, 23, 24):
            rows, size = {20: (4, 4), 21: (3, 4), 22: (4, 3), 23: (3, 3), 24: (2, 3)}[op]
            out = []
            for row in range(rows):
                rv = self.read(_shift(ins.srcs[1], row))
                out.append(sum(s[0][i] * rv[i] for i in range(size)))
            self.write(dst, out + [0.0] * (4 - len(out)))
        elif op == 32:
            self.write(dst, abs(s[0][0]) ** s[1][0] if s[0][0] else 0.0)
        elif op == 33:
            a, b = s[0], s[1]
            self.write(dst, [a[1] * b[2] - a[2] * b[1],
                             a[2] * b[0] - a[0] * b[2],
                             a[0] * b[1] - a[1] * b[0], 0.0])
        elif op == 35:
            each(abs)
        elif op == 36:
            length = math.sqrt(sum(s[0][i] ** 2 for i in range(3))) or 1.0
            self.write(dst, [s[0][i] / length for i in range(3)] + [0.0])
        elif op == 37:
            self.write(dst, [math.cos(s[0][0]), math.sin(s[0][0]), 0.0, 0.0])
        elif op == 88:
            each(lambda a, b, c: b if a >= 0.0 else c)
        elif op == 80:
            each(lambda a, b, c: b if a > 0.5 else c)
        elif op == 90:
            self.write(dst, s[0][0] * s[1][0] + s[0][1] * s[1][1] + s[2][0])
        elif op in (91, 92):
            self.write(dst, [0.0] * 4)
        elif op == 66:
            coord = s[0]
            if (ins.control & 0x3) == 1:
                w = coord[3] or 1.0
                coord = [coord[0] / w, coord[1] / w, coord[2] / w, 1.0]
            self.write(dst, self.sample(ins.srcs[1].num, coord))
        elif op in (93, 95):
            self.write(dst, self.sample(ins.srcs[1].num, s[0]))
        elif op == 65:                          # texkill reads the register in the dst slot
            val = self.r.get(dst.num, [0.0] * 4) if dst.type == TEMP \
                else self.v.get(dst.num, [0.0] * 4)
            if any(val[i] < 0.0 for i in range(3)):
                raise Discarded()
        elif op == 0:
            pass
        else:
            raise Unsupported("interpreter opcode %d" % op)


def _shift(src, delta):
    clone = src.__class__.__new__(src.__class__)
    clone.num, clone.type = src.num + delta, src.type
    clone.swizzle, clone.mod = src.swizzle, src.mod
    return clone


def compare(expected, actual, tolerance):
    for i in range(4):
        a, b = expected[i], actual[i]
        if math.isnan(a) or math.isnan(b) or math.isinf(a) or math.isinf(b):
            continue
        if abs(a) > 1e15 or abs(b) > 1e15:
            continue
        if abs(a - b) > tolerance * max(1.0, abs(a)):
            return "component %s: sm3 %.6f vs cg %.6f" % ("xyzw"[i], a, b)
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("corpus")
    ap.add_argument("--trials", type=int, default=3)
    ap.add_argument("--tolerance", type=float, default=1e-4)
    ap.add_argument("--range", type=float, default=2.0,
                    help="magnitude of the random inputs and constants")
    ap.add_argument("--show", type=int, default=8)
    args = ap.parse_args()

    names = sorted(f for f in os.listdir(args.corpus) if f.endswith((".vs", ".ps")))
    agreed = 0
    outputs_checked = 0
    problems = []

    for name in names:
        with open(os.path.join(args.corpus, name), "rb") as f:
            data = f.read()

        try:
            sh = Shader(data)
            cg_text, _ = translate(data)
        except Unsupported as e:
            problems.append((name, "translate: %s" % e))
            continue

        rng = random.Random(hash(name) & 0xFFFFFF)
        failure = None

        for _ in range(args.trials):
            span = args.range
            inputs = {n: [rng.uniform(-span, span) for _ in range(4)]
                      for (t, n) in sh.decls if t == INPUT}
            consts = {n: [rng.uniform(-span, span) for _ in range(4)]
                      for n in sh.consts if n not in sh.defs}

            sampler = make_stub(sh)
            try:
                out_regs, out_colors = Interpreter(sh, inputs, consts, sampler).run()
                discarded = False
            except Discarded:
                discarded = True
            except Unsupported as e:
                failure = "interpreter: %s" % e
                break

            variables = {"v%d" % n: Vec(vals) for n, vals in inputs.items()}
            highest = max(sh.consts) if sh.consts else -1
            variables["c"] = [Vec(list(sh.defs[i]) if i in sh.defs
                                  else consts.get(i, [0.0] * 4)) for i in range(highest + 1)]
            for n in sh.samplers:
                variables["s%d" % n] = n
                variables["volumeLayout_s%d" % n] = Vec(8.0, 0.125, 0.0, 0.0)
            for (t, n) in sh.decls:
                if t in (OUTPUT, ATTROUT):      # outputs are parameters in Cg, locals here
                    variables["o%d" % n] = Vec(0.0)

            try:
                scope = cg_eval.run(cg_text, variables,
                                    lambda unit, coord: Vec(sampler(unit, coord)))
                cg_discarded = False
            except cg_eval.Discarded:
                cg_discarded = True
            except Exception as e:
                failure = "cg eval: %s: %s" % (type(e).__name__, e)
                break

            if discarded or cg_discarded:
                if discarded != cg_discarded:
                    failure = "discard disagreement (sm3 %s, cg %s)" % (discarded, cg_discarded)
                    break
                continue

            if sh.is_vs:
                pairs = [(vals, scope.get("o%d" % reg)) for reg, vals in out_regs.items()]
            else:
                pairs = [(out_colors.get(0, [0.0] * 4), scope.get("oC0"))]

            for expected, actual in pairs:
                if actual is None:
                    failure = "no output produced by the Cg side"
                    break
                why = compare(expected, actual.v if isinstance(actual, Vec) else actual,
                              args.tolerance)
                outputs_checked += 1
                if why:
                    failure = why
                    break
            if failure:
                break

        if failure:
            problems.append((name, failure))
        else:
            agreed += 1

    print("=== %d/%d shaders agree numerically (%d outputs compared) ==="
          % (agreed, len(names), outputs_checked))
    if problems:
        print("%d disagreements:" % len(problems))
        for name, why in problems[:args.show]:
            print("  %-16s %s" % (name, why))


if __name__ == "__main__":
    main()
