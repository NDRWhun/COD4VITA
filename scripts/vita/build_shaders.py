#!/usr/bin/env python3
"""Translate a dumped SM3 corpus to Cg and compile it to GXP.

  python build_shaders.py <corpusdir> <outdir> --cgc "<path>/psp2cgc.exe"

Reports every shader that fails to translate or compile; a shader that does not
build is never silently dropped.
"""

import argparse
import os
import subprocess
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from sm3_to_cg import ALPHA_TEST_NONE, ALPHA_TEST_SUFFIX, Unsupported, translate

CGC_FLAGS = ["-O3", "-fastmath", "-fastint"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("corpus")
    ap.add_argument("outdir")
    ap.add_argument("--cgc", required=True)
    ap.add_argument("--keep-cg", action="store_true", help="keep the intermediate Cg sources")
    ap.add_argument("--no-alpha-variants", action="store_true",
                    help="bake only the alpha-test-off form of each pixel shader")
    args = ap.parse_args()

    os.makedirs(args.outdir, exist_ok=True)
    names = sorted(f for f in os.listdir(args.corpus) if f.endswith((".vs", ".ps")))

    ok = 0
    attempted_shaders = set()
    translate_fail = Counter()
    compile_fail = Counter()
    failures = []
    total_gxp = 0

    for name in names:
        stem, ext = os.path.splitext(name)

        # a pixel shader is baked once per alpha-test mode; GXM has no fixed-function test
        variants = [ALPHA_TEST_NONE] if ext == ".vs" or args.no_alpha_variants \
            else sorted(ALPHA_TEST_SUFFIX)

        with open(os.path.join(args.corpus, name), "rb") as f:
            data = f.read()

        for variant in variants:
            label = name + ALPHA_TEST_SUFFIX[variant]
            try:
                text, _ = translate(data, variant)
            except Unsupported as e:
                translate_fail[str(e)] += 1
                failures.append((label, "translate", str(e)))
                continue
            except Exception as e:
                translate_fail["%s: %s" % (type(e).__name__, e)] += 1
                failures.append((label, "translate", "%s: %s" % (type(e).__name__, e)))
                continue

            cg_path = os.path.join(args.outdir, stem + ALPHA_TEST_SUFFIX[variant] + ".cg")
            gxp_path = os.path.join(args.outdir, label + ".gxp")
            with open(cg_path, "w", newline="\n") as f:
                f.write(text)

            profile = "sce_vp_psp2" if ext == ".vs" else "sce_fp_psp2"
            r = subprocess.run([args.cgc, "-profile", profile, cg_path, "-o", gxp_path] + CGC_FLAGS,
                               capture_output=True, text=True)

            if r.returncode != 0 or not os.path.exists(gxp_path):
                first = next((l for l in (r.stderr + r.stdout).splitlines()
                              if "error" in l.lower()), "unknown")
                compile_fail[first.split(":")[-1].strip()[:70]] += 1
                failures.append((label, "compile", first.strip()))
                continue

            total_gxp += os.path.getsize(gxp_path)
            if not args.keep_cg:
                os.remove(cg_path)
            ok += 1
            attempted_shaders.add(name)

    print("=== %d variants built from %d/%d shaders ==="
          % (ok, len(attempted_shaders), len(names)))
    print("total GXP size: %.2f MB" % (total_gxp / 1048576.0))
    if ok:
        print("average GXP:    %d bytes" % (total_gxp // ok))

    if translate_fail:
        print("\n=== translation failures (%d) ===" % sum(translate_fail.values()))
        for reason, n in translate_fail.most_common():
            print("  %-60s %d" % (reason, n))
    if compile_fail:
        print("\n=== compile failures (%d) ===" % sum(compile_fail.values()))
        for reason, n in compile_fail.most_common():
            print("  %-60s %d" % (reason, n))

    if failures:
        with open(os.path.join(args.outdir, "failures.txt"), "w", newline="\n") as f:
            for name, stage, reason in failures:
                f.write("%s\t%s\t%s\n" % (name, stage, reason))
        print("\nper-shader detail in %s" % os.path.join(args.outdir, "failures.txt"))


if __name__ == "__main__":
    main()
