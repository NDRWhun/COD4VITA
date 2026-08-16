#!/usr/bin/env python3
"""Count how many translation units reach a given header through their includes.

Used to measure what a Vita build has to get past: every .cpp whose include closure
pulls in d3d9.h or windows.h has to compile without them.

  python include_closure.py <srcdir> [--header d3d9.h] [--list] [--exclude src/win32,src/radiant]
"""

import argparse
import os
import re
import sys

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]', re.MULTILINE)
COND_RE = re.compile(r'^\s*#\s*(ifdef|ifndef|if|else|elif|endif)\b(.*)$')

DEFINED = set()


def includes_of(text):
    """Include names in branches active for the current define set."""
    out = []
    stack = []                  # True where the branch is taken
    for line in text.splitlines():
        cond = COND_RE.match(line)
        if cond:
            kind, rest = cond.group(1), cond.group(2).strip()
            if kind == "ifdef":
                stack.append(rest.split()[0] in DEFINED if rest else True)
            elif kind == "ifndef":
                stack.append(rest.split()[0] not in DEFINED if rest else True)
            elif kind == "if":
                m = re.fullmatch(r'defined\s*\(?\s*(\w+)\s*\)?', rest)
                stack.append(m.group(1) in DEFINED if m else True)
            elif kind == "elif":
                if stack:
                    stack[-1] = not stack[-1]
            elif kind == "else":
                if stack:
                    stack[-1] = not stack[-1]
            elif kind == "endif":
                if stack:
                    stack.pop()
            continue
        if all(stack):
            m = INCLUDE_RE.match(line)
            if m:
                out.append(m.group(2))
    return out


def read(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def resolve(name, from_dir, roots):
    candidates = [os.path.join(from_dir, name)] + [os.path.join(r, name) for r in roots]
    for candidate in candidates:
        if os.path.isfile(candidate):
            return os.path.normpath(candidate)
    return None


def closure(path, roots, cache, stack=None):
    """Set of resolved headers reachable from path, plus unresolved include names."""
    path = os.path.normpath(path)
    if path in cache:
        return cache[path]

    stack = stack or set()
    if path in stack:
        return set()

    cache[path] = set()            # placeholder against cycles
    stack = stack | {path}

    reached = set()
    from_dir = os.path.dirname(path)
    for name in includes_of(read(path)):
        target = resolve(name, from_dir, roots)
        if target:
            reached.add(target)
            reached |= closure(target, roots, cache, stack)
        else:
            reached.add(name.lower())      # a system header we do not have on disk

    cache[path] = reached
    return reached


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("srcdir")
    ap.add_argument("--header", default="d3d9.h")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--exclude", default="", help="comma separated path fragments to skip")
    ap.add_argument("--define", default="", help="comma separated macros to treat as defined")
    args = ap.parse_args()

    DEFINED.update(d.strip() for d in args.define.split(",") if d.strip())

    roots = [args.srcdir]
    excluded = [e.strip().replace("\\", "/") for e in args.exclude.split(",") if e.strip()]

    units = []
    for base, _dirs, files in os.walk(args.srcdir):
        for name in files:
            if name.endswith((".cpp", ".c")):
                units.append(os.path.join(base, name))

    cache = {}
    hits = []
    skipped = 0
    needle = args.header.lower()

    for unit in sorted(units):
        rel = os.path.relpath(unit).replace("\\", "/")
        if any(x in rel for x in excluded):
            skipped += 1
            continue
        reached = closure(unit, roots, cache)
        if any(needle == os.path.basename(str(r)).lower() for r in reached):
            hits.append(rel)

    print("%s: reached by %d of %d translation units (%d skipped)"
          % (args.header, len(hits), len(units) - skipped, skipped))
    if args.list:
        for h in hits:
            print("  %s" % h)
    return 0


if __name__ == "__main__":
    sys.exit(main())
