#!/usr/bin/env python3
"""Generate the d3d9 shim header from the real SDK headers.

The Vita build has no d3d9.h, but engine headers name its types and constants. Rather
than transcribe values by hand, this reads them out of the installed DirectX SDK and
emits explicit definitions for exactly the symbols the engine uses.

  python gen_d3d9_shim.py --sdk "C:/Program Files (x86)/Microsoft DirectX SDK (June 2010)/Include"
"""

import argparse
import os
import re
import sys

ENUM_RE = re.compile(r'typedef\s+enum\s+(\w+)?\s*\{(.*?)\}\s*(\w+)\s*;', re.S)
STRUCT_RE = re.compile(r'typedef\s+struct\s+(\w+)\s*\{(.*?)\}\s*([\w\s,\*]+);', re.S)


def parse_structs(source):
    """struct tag -> (body, [alias names]); bodies are copied as declared."""
    out = {}
    for match in STRUCT_RE.finditer(source):
        tag = match.group(1)
        body = match.group(2)
        aliases = [a.strip().lstrip('*') for a in match.group(3).split(',') if a.strip()]
        out[tag] = (body, aliases)
    return out


FOURCC_RE = re.compile(r"MAKEFOURCC\s*\(\s*'(.)'\s*,\s*'(.)'\s*,\s*'(.)'\s*,\s*'(.)'\s*\)")
HRESULT_RE = re.compile(r"MAKE_D3DHRESULT\s*\(\s*(\d+)\s*\)")


def expand_macros(text):
    """MAKEFOURCC and MAKE_D3DHRESULT hold commas, which would break entry splitting."""
    def fourcc(m):
        a, b, c, d = (ord(x) for x in m.groups())
        return str(a | (b << 8) | (c << 16) | (d << 24))

    text = FOURCC_RE.sub(fourcc, text)
    # MAKE_D3DHRESULT(code) is 0x80000000 | (_FACD3D << 16) | code, _FACD3D being 0x876
    return HRESULT_RE.sub(lambda m: str(0x88760000 | int(m.group(1))), text)


def evaluate(text, known):
    text = text.strip().rstrip('lLuU')
    if text in known:
        return known[text]
    try:
        return int(eval(text, {"__builtins__": {}}, dict(known)))
    except Exception:
        return None


def parse_enums(source):
    """enum tag -> (typedef name, {symbol: value}), resolving implicit numbering."""
    out = {}
    for match in ENUM_RE.finditer(source):
        tag = match.group(1) or match.group(3)
        alias = match.group(3)
        body = re.sub(r'/\*.*?\*/', '', match.group(2), flags=re.S)
        body = re.sub(r'//[^\n]*', '', body)
        body = expand_macros(body)

        values = {}
        nxt = 0
        for entry in body.split(','):
            entry = entry.strip()
            if not entry:
                continue
            if '=' in entry:
                symbol, expr = entry.split('=', 1)
                symbol = symbol.strip()
                value = evaluate(expr, values)
                if value is None:
                    continue
            else:
                symbol, value = entry, nxt
            values[symbol.strip()] = value
            nxt = value + 1
        out[tag] = (alias, values)
    return out


def collect_defines(source):
    out = {}
    for m in re.finditer(r'^\s*#define\s+(D3D\w+)\s+([^\n]+)', source, re.M):
        name = m.group(1)
        expr = re.sub(r'/\*.*', '', m.group(2))
        expr = re.sub(r'//.*', '', expr).strip()
        expr = expand_macros(expr)
        try:
            value = int(expr.rstrip('lLuU'), 0)
        except ValueError:
            value = evaluate(expr, out)
        if value is not None:
            out[name] = value
    return out


def used_symbols(srcdir):
    pattern = re.compile(r'\b(?:IDirect3D\w*|D3D[A-Z_][A-Za-z0-9_]*|_D3D[A-Za-z0-9_]*)\b')
    found = set()
    for base, _dirs, files in os.walk(srcdir):
        rel = base.replace('\\', '/')
        if '/win32' in rel or '/radiant' in rel:
            continue
        for name in files:
            if not name.endswith(('.cpp', '.h')) or name == 'd3d9_shim.h':
                continue
            with open(os.path.join(base, name), encoding='utf-8', errors='replace') as f:
                found.update(pattern.findall(f.read()))
    return found


INTERFACES = [
    "IDirect3D9", "IDirect3DDevice9", "IDirect3DBaseTexture9", "IDirect3DTexture9",
    "IDirect3DVolumeTexture9", "IDirect3DCubeTexture9", "IDirect3DVertexBuffer9",
    "IDirect3DIndexBuffer9", "IDirect3DVertexShader9", "IDirect3DPixelShader9",
    "IDirect3DVertexDeclaration9", "IDirect3DSurface9", "IDirect3DQuery9",
    "IDirect3DSwapChain9",
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sdk", required=True)
    ap.add_argument("--src", default="src")
    ap.add_argument("--out", default="src/gfx_d3d/d3d9_shim.h")
    args = ap.parse_args()

    source = ""
    for name in ("d3d9types.h", "d3d9caps.h", "d3d9.h"):
        path = os.path.join(args.sdk, name)
        if os.path.isfile(path):
            with open(path, encoding="utf-8", errors="replace") as f:
                source += f.read() + "\n"
    if not source:
        sys.exit("no d3d9 headers found under %s" % args.sdk)

    enums = parse_enums(source)
    defines = collect_defines(source)
    wanted = used_symbols(args.src)

    lines = [
        "// D3D9 declarations the engine's headers need, for targets without the SDK.",
        "// Generated by scripts/vita/gen_d3d9_shim.py -- do not edit.",
        "#pragma once",
        "",
        "#include <stdint.h>",
        "",
        "#include <qcommon/sys_types.h>",
        "",
        "typedef int32_t HRESULT;",
        "typedef uint32_t D3DCOLOR;",
        "",
        "#define D3D_OK 0",
        "#define MAX_DEVICE_IDENTIFIER_STRING 512",
        "",
        "typedef struct _GUID",
        "{",
        "    uint32_t Data1;",
        "    uint16_t Data2;",
        "    uint16_t Data3;",
        "    unsigned char Data4[8];",
        "} GUID;",
        "",
    ]

    for name in INTERFACES:
        if name in wanted:
            lines.append("struct %s;" % name)
    lines.append("")

    emitted = set()
    for tag in sorted(enums):
        alias, values = enums[tag]
        members = {s: v for s, v in values.items() if s in wanted}
        if not members and tag not in wanted and alias not in wanted:
            continue
        lines.append("enum %s" % tag)
        lines.append("{")
        for symbol, value in sorted(members.items(), key=lambda kv: kv[1]):
            lines.append("    %-34s = %s," % (symbol, ("0x%08X" % value) if value > 9999 else value))
            emitted.add(symbol)
        lines.append("};")
        if alias and alias != tag:
            lines.append("typedef enum %s %s;" % (tag, alias))
            emitted.add(alias)
        emitted.add(tag)
        lines.append("")

    structs = parse_structs(source)
    alias_to_tag = {a: tag for tag, (_b, aliases) in structs.items() for a in aliases}

    # a wanted struct drags in whatever its fields name, so close over that first
    needed = {t for t in structs if t in wanted or any(a in wanted for a in structs[t][1])}
    while True:
        grown = set(needed)
        for tag in needed:
            for word in re.findall(r'\b\w+\b', structs[tag][0]):
                target = word if word in structs else alias_to_tag.get(word)
                if target:
                    grown.add(target)
        if grown == needed:
            break
        needed = grown

    # dependencies before dependants
    order = []
    remaining = set(needed)
    while remaining:
        for tag in sorted(remaining):
            deps = set()
            for word in re.findall(r'\b\w+\b', structs[tag][0]):
                target = word if word in structs else alias_to_tag.get(word)
                if target and target != tag:
                    deps.add(target)
            if not (deps & remaining):
                order.append(tag)
                remaining.discard(tag)
                break
        else:
            order.extend(sorted(remaining))     # a cycle; emit what is left
            break

    for tag in order:
        body, aliases = structs[tag]
        lines.append("struct %s" % tag)
        lines.append("{")
        for line in body.strip().splitlines():
            text = re.sub(r'/\*.*?\*/', '', line).rstrip()
            if text.strip():
                lines.append("    " + text.strip())
        lines.append("};")
        for alias in aliases:
            if alias != tag:
                lines.append("typedef struct %s %s;" % (tag, alias))
                emitted.add(alias)
        emitted.add(tag)
        lines.append("")

    extra = sorted(s for s in wanted if s in defines and s not in emitted)
    if extra:
        for symbol in extra:
            value = defines[symbol]
            lines.append("#define %-34s %s" % (symbol, ("0x%08X" % value) if value > 9999 else value))
            emitted.add(symbol)
        lines.append("")

    with open(args.out, "w", newline="\n", encoding="utf-8") as f:
        f.write("\n".join(lines))

    missing = sorted(s for s in wanted
                     if s not in emitted and s not in INTERFACES and not s.startswith("_D3D"))
    print("wrote %s: %d interfaces, %d constants" % (args.out, len(INTERFACES), len(emitted)))
    print("engine references %d D3D symbols; %d not found in the SDK headers" % (len(wanted), len(missing)))
    if missing:
        print("  " + ", ".join(missing[:24]))


if __name__ == "__main__":
    main()
