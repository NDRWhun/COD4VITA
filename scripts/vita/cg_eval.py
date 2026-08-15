"""Evaluate the Cg subset emitted by sm3_to_cg.py, for differential testing.

A float4 with swizzle attributes and operator overloading makes the generated Cg
close enough to Python that only braces, declarations and the ternary need
rewriting. Nothing here is used at runtime -- it exists to check the translator.
"""

import math
import re

COMPONENTS = "xyzw"


class Vec(object):
    __slots__ = ("v",)

    def __init__(self, *args):
        vals = []
        for a in args:
            if isinstance(a, Vec):
                vals.extend(a.v)
            elif isinstance(a, (list, tuple)):
                vals.extend(float(x) for x in a)
            else:
                vals.append(float(a))
        if len(vals) == 1:
            vals = vals * 4
        object.__setattr__(self, "v", (vals + [0.0, 0.0, 0.0])[:4])

    def __getattr__(self, name):
        if name and all(c in COMPONENTS for c in name):
            picked = [self.v[COMPONENTS.index(c)] for c in name]
            return picked[0] if len(picked) == 1 else Vec(picked)
        raise AttributeError(name)

    def __setattr__(self, name, value):
        if name == "v" or not all(c in COMPONENTS for c in name):
            object.__setattr__(self, name, value)
            return
        src = _wide(value)
        target = list(self.v)
        for i, c in enumerate(name):
            target[COMPONENTS.index(c)] = src[i] if len(name) > 1 else src[0]
        object.__setattr__(self, "v", target)

    def _apply(self, other, fn):
        b = _wide(other)
        return Vec([fn(a, b[i]) for i, a in enumerate(self.v)])

    def __add__(self, o):
        return self._apply(o, lambda a, b: a + b)

    def __radd__(self, o):
        return Vec(_wide(o))._apply(self, lambda a, b: a + b)

    def __sub__(self, o):
        return self._apply(o, lambda a, b: a - b)

    def __rsub__(self, o):
        return Vec(_wide(o))._apply(self, lambda a, b: a - b)

    def __mul__(self, o):
        return self._apply(o, lambda a, b: a * b)

    __rmul__ = __mul__

    def __truediv__(self, o):
        return self._apply(o, lambda a, b: a / b if b else 0.0)

    def __rtruediv__(self, o):
        return Vec(_wide(o))._apply(self, lambda a, b: a / b if b else 0.0)

    def __neg__(self):
        return Vec([-a for a in self.v])

    def __lt__(self, o):
        return self._apply(o, lambda a, b: 1.0 if a < b else 0.0)

    def __le__(self, o):
        return self._apply(o, lambda a, b: 1.0 if a <= b else 0.0)

    def __gt__(self, o):
        return self._apply(o, lambda a, b: 1.0 if a > b else 0.0)

    def __ge__(self, o):
        return self._apply(o, lambda a, b: 1.0 if a >= b else 0.0)

    def __eq__(self, o):
        return self._apply(o, lambda a, b: 1.0 if a == b else 0.0)

    def __ne__(self, o):
        return self._apply(o, lambda a, b: 1.0 if a != b else 0.0)

    def __repr__(self):
        return "Vec(%s)" % ", ".join("%.5f" % x for x in self.v)


def _wide(x, n=4):
    if isinstance(x, Vec):
        return list(x.v)
    if isinstance(x, (list, tuple)):
        vals = [float(v) for v in x]
        return (vals + [vals[-1]] * n)[:n]
    return [float(x)] * n


def _scalar(x):
    return x.v[0] if isinstance(x, Vec) else float(x)


def _lift(fn):
    def wrapped(*args):
        # Cg's overloads keep a scalar call scalar; widening it here would turn
        # float4(cos(a), sin(a), 0, 0) into four copies of the first component
        if not any(isinstance(a, (Vec, list, tuple)) for a in args):
            return fn(*[float(a) for a in args])
        wide = [_wide(a) for a in args]
        return Vec([fn(*[w[i] for w in wide]) for i in range(4)])
    return wrapped


def _dot(a, b, width=4):
    wa, wb = _wide(a), _wide(b)
    n = width
    if isinstance(a, Vec) and isinstance(b, Vec):
        n = width
    return sum(wa[i] * wb[i] for i in range(n))


def _normalize(v):
    w = _wide(v)
    length = math.sqrt(w[0] ** 2 + w[1] ** 2 + w[2] ** 2) or 1.0
    return Vec([w[0] / length, w[1] / length, w[2] / length, 0.0])


def _cross(a, b):
    x, y = _wide(a), _wide(b)
    return Vec([x[1] * y[2] - x[2] * y[1],
                x[2] * y[0] - x[0] * y[2],
                x[0] * y[1] - x[1] * y[0], 0.0])


def _select(cond, a, b):
    c, wa, wb = _wide(cond), _wide(a), _wide(b)
    return Vec([wa[i] if c[i] else wb[i] for i in range(4)])


def _any(v):
    return any(x for x in _wide(v))


class Discarded(Exception):
    pass


def environment(sampler):
    """Intrinsics for the generated subset; sampler(unit, coord) supplies texture reads."""
    return {
        "Vec": Vec,
        "float4": Vec, "float3": Vec, "float2": Vec, "float": float,
        "saturate": _lift(lambda x: min(1.0, max(0.0, x))),
        "abs": _lift(abs),
        "min": _lift(min), "max": _lift(max),
        "lerp": _lift(lambda a, b, t: a + t * (b - a)),
        "frac": _lift(lambda x: x - math.floor(x)),
        "exp2": _lift(lambda x: 2.0 ** x if x < 128 else float("inf")),
        "log2": _lift(lambda x: math.log(x, 2) if x > 0 else -1e30),
        "rsqrt": _lift(lambda x: 1.0 / math.sqrt(x) if x > 0 else 0.0),
        "pow": _lift(lambda a, b: a ** b if a > 0 else 0.0),
        "cos": _lift(math.cos), "sin": _lift(math.sin),
        "floor": _lift(math.floor),
        "clamp": _lift(lambda x, lo, hi: min(hi, max(lo, x))),
        "ddx": lambda v: Vec(0.0), "ddy": lambda v: Vec(0.0),
        "dot": _dot,
        "normalize": _normalize,
        "cross": _cross,
        "any": _any,
        "select": _select,
        "discard": Discarded,
        "tex2D": sampler, "texCUBE": sampler, "tex3D": sampler,
        "tex2Dproj": lambda s, c: sampler(s, _project(c)),
        "texCUBEproj": lambda s, c: sampler(s, _project(c)),
        "tex2Dlod": lambda s, c: sampler(s, c),
        "texCUBElod": lambda s, c: sampler(s, c),
        "tex2Dbias": lambda s, c: sampler(s, c),
        "sampleVolume": lambda s, uvw, layout: sampler(s, uvw),
    }


def _project(c):
    w = _wide(c)
    d = w[3] or 1.0
    return Vec([w[0] / d, w[1] / d, w[2] / d, 1.0])


def _split_top(text, marks):
    """Positions of the given characters at paren depth zero."""
    found, depth = {}, 0
    for i, c in enumerate(text):
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
        elif depth == 0 and c in marks and c not in found:
            found[c] = i
    return found


def _expression(text):
    """Rewrite `cond ? a : b` as select(), innermost first, respecting parentheses."""
    text = text.strip()
    while True:
        q = text.find("?")
        if q < 0:
            return text

        # the group holding this ternary: the enclosing parens, else the whole expression
        depth, start = 0, None
        for i in range(q, -1, -1):
            if text[i] == ")":
                depth += 1
            elif text[i] == "(":
                if depth == 0:
                    start = i
                    break
                depth -= 1

        if start is None:
            inner, prefix, suffix = text, "", ""
        else:
            depth, end = 0, len(text)
            for i in range(start + 1, len(text)):
                if text[i] == "(":
                    depth += 1
                elif text[i] == ")":
                    if depth == 0:
                        end = i
                        break
                    depth -= 1
            inner = text[start + 1:end]
            prefix, suffix = text[:start + 1], text[end:]

        marks = _split_top(inner, "?:")
        if "?" not in marks or ":" not in marks:
            return text
        cond = inner[:marks["?"]]
        a = inner[marks["?"] + 1:marks[":"]]
        b = inner[marks[":"] + 1:]
        replaced = "select(%s, %s, %s)" % (cond.strip(), a.strip(), b.strip())
        text = prefix + replaced + suffix if start is not None else replaced


def to_python(cg_text):
    """Rewrite one generated Cg function body as Python source."""
    start = cg_text.index("{", cg_text.index("main"))
    body = cg_text[start + 1:cg_text.rindex("}")]

    out = []
    depth = 0                   # run() adds the function's own indent
    for raw in body.splitlines():
        line = raw.strip()
        if not line or line.startswith("//"):
            continue

        if line == "}":
            depth -= 1
            continue
        if line == "} else {":
            out.append("    " * (depth - 1) + "else:")
            continue

        opens = line.endswith("{")
        if opens:
            line = line[:-1].strip()

        if line.startswith("if ("):
            cond = line[len("if ("):line.rindex(")")]
            if "discard" in raw:
                out.append("    " * depth + "if %s: raise discard()" % _expression(cond))
                continue
            out.append("    " * depth + "if %s:" % _expression(cond))
            depth += 1
            continue

        if line.startswith("for ("):
            var = re.search(r"int\s+(\w+)", line).group(1)
            count = re.search(r"<\s*(\d+)", line).group(1)
            out.append("    " * depth + "for %s in range(%s):" % (var, count))
            depth += 1
            continue

        line = line.rstrip(";")
        if line.startswith("return"):
            continue

        if line.startswith("float4 ") or line.startswith("const float4 "):
            decl = line.replace("const ", "").replace("float4 ", "", 1)
            if "=" in decl:
                name, value = decl.split("=", 1)
                out.append("    " * depth + "%s = %s" % (name.strip(), _expression(value)))
            else:
                for name in decl.split(","):
                    out.append("    " * depth + "%s = Vec(0.0)" % name.strip())
            continue

        lhs, rhs = line.split("=", 1)
        lhs = lhs.strip()
        if "." in lhs:
            out.append("    " * depth + "%s = %s" % (lhs, _expression(rhs)))
        else:
            # an unmasked Cg assignment copies; plain Python would alias the vector
            out.append("    " * depth + "%s = Vec(%s)" % (lhs, _expression(rhs)))

    return "\n".join(out)


def run(cg_text, variables, sampler):
    """Execute the translated shader and return its final variable scope."""
    python = to_python(cg_text)
    source = "def _shader():\n"
    source += "\n".join("    " + line for line in python.splitlines())
    source += "\n    return locals()\n"

    env = environment(sampler)
    env.update(variables)
    scope = dict(env)
    code = compile(source, "<cg>", "exec")
    exec(code, scope)

    # a masked write mutates a passed-in vector in place and never becomes a local,
    # so the caller's view and the function's locals both matter
    result = dict(variables)
    result.update(scope["_shader"]())
    return result
