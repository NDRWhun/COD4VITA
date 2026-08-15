#!/usr/bin/env python3
"""Convert the game's Bink cinematics to H.264 for the Vita's video decoder.

Bink is a proprietary x86 codec with no Vita build, so the videos are re-encoded
offline and played back through sceAvPlayer. 720p is downscaled to the panel's
960x544, which is where most of the size saving comes from.

  python convert_video.py "<cod4>/main/video" <outdir> --ffmpeg <path>
"""

import argparse
import os
import subprocess
import sys

# the panel is 960x544; -2 keeps the height even for the encoder
SCALE = "scale=960:-2:flags=lanczos"


def convert(ffmpeg, src, dst, crf, audio_rate):
    cmd = [
        ffmpeg, "-y", "-loglevel", "error", "-i", src,
        "-vf", SCALE,
        "-c:v", "libx264",
        "-profile:v", "main", "-level", "3.1",   # what the hardware decoder accepts
        "-pix_fmt", "yuv420p",
        "-crf", str(crf),
        "-c:a", "aac", "-b:a", "%dk" % audio_rate, "-ac", "2",
        "-movflags", "+faststart",
        dst,
    ]
    result = subprocess.run(cmd, capture_output=True, text=True)
    return result.returncode == 0, result.stderr.strip()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("source")
    ap.add_argument("outdir")
    ap.add_argument("--ffmpeg", default="ffmpeg")
    ap.add_argument("--crf", type=int, default=23)
    ap.add_argument("--audio-rate", type=int, default=96)
    args = ap.parse_args()

    os.makedirs(args.outdir, exist_ok=True)
    names = sorted(n for n in os.listdir(args.source) if n.lower().endswith(".bik"))
    if not names:
        sys.exit("no .bik files in %s" % args.source)

    before = after = 0
    failures = []

    for name in names:
        src = os.path.join(args.source, name)
        dst = os.path.join(args.outdir, os.path.splitext(name)[0] + ".mp4")
        ok, error = convert(args.ffmpeg, src, dst, args.crf, args.audio_rate)
        if not ok:
            failures.append((name, error.splitlines()[-1] if error else "unknown"))
            print("%-32s FAILED" % name)
            continue

        src_size, dst_size = os.path.getsize(src), os.path.getsize(dst)
        before += src_size
        after += dst_size
        print("%-32s %7.1f MB -> %6.1f MB" % (name, src_size / 1048576.0, dst_size / 1048576.0))

    print("\n=== %d converted, %d failed ===" % (len(names) - len(failures), len(failures)))
    if before:
        print("%.2f GB -> %.2f GB (%.0f%% of the original)"
              % (before / 1073741824.0, after / 1073741824.0, 100.0 * after / before))
    for name, error in failures:
        print("  %-32s %s" % (name, error))


if __name__ == "__main__":
    main()
