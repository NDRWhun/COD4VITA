# Building and asset preparation

Everything the game needs is either in this repository or made from your own copy of
Call of Duty 4 (PC). Nothing game-derived is distributed.

## Tools

| Tool | For |
| --- | --- |
| [VitaSDK](https://vitasdk.org) (`VITASDK` set, `$VITASDK/bin` on PATH) | building the VPK |
| CMake + ninja (or make) | build system |
| Python 3 | every asset script |
| ffmpeg | video re-encode |
| Sony's `psp2cgc.exe` (you supply it) | shader compilation |
| Call of Duty 4 (PC) install | all game assets |

On Windows, run everything from Git Bash.

## Build the VPK

```bash
bash tools/build.sh                                        # -> build/COD4VITA.vpk
bash tools/build.sh -DSHADER_ARCHIVE=/path/shaders.kgxp    # bundle your shader archive
```

Without `SHADER_ARCHIVE` the VPK builds with fallback shaders — fine for CI, wrong for
playing. Build the archive once (below) and either bundle it or copy it to the card.

## Asset preparation (once, on your PC)

All scripts live in `scripts/vita/`. `<cod4>` is your install directory.

**Shaders** — extract every SM3 shader from the fastfiles, translate to Cg, compile with
`psp2cgc`, pack into one archive the runtime looks up by bytecode hash:

```bash
python ff_shader_scan.py "<cod4>/zone/english" corpus/
python build_shaders.py corpus/ gxp/ --cgc /path/to/psp2cgc.exe
python pack_shaders.py gxp/ shaders.kgxp
```

**Textures** — cap streamed textures at 512px and repack the archives (about half the size):

```bash
python iwi_strip.py "<cod4>/main" stripped/
```

**Video** — re-encode the Bink cinematics to H.264/AAC for the hardware decoder:

```bash
python convert_video.py "<cod4>/main/video" video/ --ffmpeg /path/to/ffmpeg
```

**LiveArea** (optional) — generate the bubble artwork from your own images before building:

```bash
python make_livearea.py --background yourbg.png --logo yourlogo.png
```

## Card layout

```
ux0:data/kisakcod/
  zone/english/        <- copied from <cod4>/zone/english
  main/                <- the stripped .iwd archives
  video/               <- the converted .mp4 files
  shaders.kgxp         <- unless bundled into the VPK
  raw/vita_controls.cfg   (written on first boot; edit to rebind)
```

Install `build/COD4VITA.vpk` with VitaShell and play. Logs land in
`ux0:data/kisakcod/kisakcod.log`.

## Other scripts

`gen_d3d9_shim.py` regenerates the committed `src/gfx_d3d/d3d9_shim.h` from the DirectX SDK
headers — only needed if that interface changes. `verify_*.py`, `cg_eval.py` and `sm3_*.py`
are the shader translator's offline test rig. `iwi_scan.py` reports texture statistics.
