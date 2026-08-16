# Third-party components

This port is GPLv3, inherited from [KisakCOD](https://github.com/SwagSoftware/KisakCOD). The list
below covers what the tree carries and what the build reaches for but does not ship.

## Vendored in `deps/`

Inherited from upstream KisakCOD. The Vita target uses none of them except zlib.

| Component | License | Where | Role |
| --- | --- | --- | --- |
| zlib 1.1.4 | [zlib](https://zlib.net/zlib_license.html) | `deps/zlib/` | fastfile decompression; the only one the Vita target links |
| ODE | [BSD-3 or LGPL-2.1](https://bitbucket.org/odedevs/ode/src/master/LICENSE.TXT) | `deps/ode/` | physics interface headers only |
| Speex | [BSD-3 (Xiph)](https://gitlab.xiph.org/xiph/speex/-/blob/master/COPYING) | `deps/speex/` | voice codec headers only |
| dr_libs | public domain or MIT-0 | `deps/dr_libs/` | WAV and MP3 decoders |
| Bink | proprietary (RAD Game Tools) | `deps/binklib/` | video playback on Windows; replaced on Vita |
| Miles Sound System | proprietary (RAD Game Tools) | `deps/msslib/` | audio on Windows; excluded from the Vita target |
| Steamworks SDK | proprietary (Valve) | `deps/steamsdk/` | Steam integration; unused on Vita |

Only dr_libs states its licence in its own source. The other vendored copies arrived without licence
files; the table links the upstream text for each.

The three proprietary entries include compiled libraries (`.lib`, `.dll`, `.so`, `.asi`). They are
inherited from upstream and are not redistributable under their vendors' terms — see the note at
the end of this file.

## Build-time only, not included

| Component | License | Role |
| --- | --- | --- |
| VitaSDK | mixed, mostly MIT/BSD | ARM toolchain and homebrew import stubs; installed separately |
| Sony PSVita SDK (`psp2cgc`) | proprietary (Sony) | compiles translated shaders to GXP; not redistributable |
| DirectX SDK (June 2010) | proprietary (Microsoft) | source for the generated `d3d9_shim.h`; already a KisakCOD requirement |
| Pillow | [MIT-CMU](https://github.com/python-pillow/Pillow/blob/main/LICENSE) | used by `make_livearea.py` |

No Sony SDK code ships. The runtime links VitaSDK's import stubs, which contain no Sony source.

## Game assets

Not distributed, in any form. Everything derived from Call of Duty 4 is generated on the builder's
own machine from a legally-owned copy:

- `shaders.kgxp` is built from shader bytecode extracted out of the local install's fastfiles.
- LiveArea artwork is built by `scripts/vita/make_livearea.py` from images the builder supplies.
- `src/gfx_d3d/d3d9_shim.h` is generated from the local DirectX SDK headers.

All three are gitignored.

## Note on the vendored proprietary libraries

`deps/binklib`, `deps/msslib` and `deps/steamsdk` contain vendor-licensed binaries carried over from
upstream KisakCOD. The Vita target does not build against any of them: sound is excluded, video goes
through the GXM path in `src/vita/gxm/gxm_video.cpp`, and there is no Steam integration. They remain
in the tree only so the upstream Windows targets still build.

## Trademarks

*Call of Duty* and *Modern Warfare* are trademarks of Activision Publishing, Inc. *PlayStation Vita*
is a trademark of Sony Interactive Entertainment. Used here only to identify the game and the
hardware. This project is unofficial and not affiliated with, endorsed by, or supported by either.
