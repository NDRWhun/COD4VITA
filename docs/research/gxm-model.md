# GXM model — what the backend is built on

Derived from the official vendor docs (`libgxm-Overview_e.pdf`, `libgxm-Reference_e.pdf`,
`GPU-Users_Guide_e.pdf`) and the SDK headers. Citations are section names in those documents.

## Memory

CPU and GPU see the same virtual address, so a mapped pointer is usable in GPU structures
directly. (Overview §10, "Memory Model")

Allocation is `sceKernelAllocMemBlock`, then `sceGxmMapMemory` to make it GPU-visible:

| Block type | CPU | Granularity | Used for |
| --- | --- | --- | --- |
| `USER_RW` | cached, GPU snoops CPU caches | 4 KB | CPU-written vertex/index data |
| `USER_RW_UNCACHE` | uncached | 4 KB | write-once streaming data |
| `USER_CDRAM_RW` | uncached | 256 KB | render targets, textures |

The GPU always reads and writes *cached*, whichever block type is used; the block type only
decides CPU-side caching. Map read-only whatever the GPU never writes (textures, vertex data) so
the hardware traps stray writes; colour/depth surfaces and writable uniform buffers must be
read-write.

Shader code is mapped separately with `sceGxmMapVertexUsseMemory` /
`sceGxmMapFragmentUsseMemory`, which return a **USSE offset**, not a pointer — the shader cores
cannot address memory by CPU virtual address. One region mapped as both gets two different
offsets. Unmap before free, and only when the GPU is idle (`sceGxmFinish`).

## Shaders

`psp2cgc` output is a `SceGxmProgram` blob; validate with `sceGxmProgramCheck`. Parameters come
back in four categories (Overview §11): `ATTRIBUTE` (resource index = vertex attribute register),
`UNIFORM` (resource index = 32-bit word offset in its buffer; container index names the buffer, or
invalid for the default buffer), `SAMPLER` (resource index = texture unit), `UNIFORM_BUFFER`.

The default uniform buffer has no parameter entry — size comes from
`sceGxmProgramGetDefaultUniformBufferSize`.

## Shader patcher — the important one

The patcher turns compiler output into final programs, generating extra USSE code for state that
other APIs treat as dynamic (Overview §12). It needs four allocators: host (plain `malloc`),
buffer (GPU read-write, for literals and register spills), vertex USSE, fragment USSE. Vertex and
fragment USSE may share one region only if base and size match exactly — the two mappings still
return different offsets.

**Vertex programs** are created from attribute descriptions plus stream descriptions. The
attribute descriptions generate unpack code from the in-memory format to float; missing components
are filled from `(0,0,0,1)`. Streams carry the stride and an index source (`INDEX_16BIT`,
`INDEX_32BIT`, or the `INSTANCE_*` variants); 32-bit sources generate more expensive PDS indexing,
so use U16 unless the mesh genuinely needs more.

**Fragment programs** bake in: output register format, MSAA mode, **blend mode and colour mask**,
and optionally the vertex program to link against (which remaps texcoords across gaps in the
vertex program's outputs). Blending only works when the output register format has four
components — `UCHAR4` or `HALF4`; anything else errors out at creation.

This is the design constraint for the whole backend: in D3D9 the blend state is a render-state
bit, here it is part of the compiled program. Blend/mask permutations must be enumerated and
created up front. Programs are refcounted and deduplicated — creating with identical arguments
returns the existing program with its refcount raised — so a create-on-demand cache is cheap, but
the creation itself is not something to do inside a frame.

## Tiler behaviour

SGX is tile-based deferred; hidden-surface removal looks ahead for opaque geometry and skips
occluded pixels, but does **not** sort. Submission order matters (Overview §13): all opaque first,
then anything using `discard` or fragment depth writes, then alpha-blended geometry sorted by
depth as usual. Overdraw inside a tile is cheap; discard and framebuffer reads are not.
