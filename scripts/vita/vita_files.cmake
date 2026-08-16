set(VITA_PLATFORM
    "${SRC_DIR}/vita/platform/vita_files.cpp"
    "${SRC_DIR}/vita/platform/vita_files.h"
    "${SRC_DIR}/vita/platform/vita_memory.cpp"
    "${SRC_DIR}/vita/platform/vita_memory.h"
    "${SRC_DIR}/vita/platform/vita_prefix.h"
    "${SRC_DIR}/vita/platform/vita_selftest.cpp"
    "${SRC_DIR}/vita/platform/vita_selftest.h"
    "${SRC_DIR}/vita/platform/vita_sys.cpp"
    "${SRC_DIR}/vita/platform/vita_sys.h"
    "${SRC_DIR}/vita/platform/vita_threads.cpp"
    "${SRC_DIR}/vita/platform/vita_threads.h"
)

set(VITA_GXM
    "${SRC_DIR}/vita/gxm/gxm_buffer.cpp"
    "${SRC_DIR}/vita/gxm/gxm_buffer.h"
    "${SRC_DIR}/vita/gxm/gxm_device.cpp"
    "${SRC_DIR}/vita/gxm/gxm_device.h"
    "${SRC_DIR}/vita/gxm/gxm_draw.cpp"
    "${SRC_DIR}/vita/gxm/gxm_draw.h"
    "${SRC_DIR}/vita/gxm/gxm_memory.cpp"
    "${SRC_DIR}/vita/gxm/gxm_memory.h"
    "${SRC_DIR}/vita/gxm/gxm_program.cpp"
    "${SRC_DIR}/vita/gxm/gxm_program.h"
    "${SRC_DIR}/vita/gxm/gxm_shader_archive.cpp"
    "${SRC_DIR}/vita/gxm/gxm_shader_archive.h"
    "${SRC_DIR}/vita/gxm/gxm_state.cpp"
    "${SRC_DIR}/vita/gxm/gxm_state.h"
    "${SRC_DIR}/vita/gxm/gxm_texture.cpp"
    "${SRC_DIR}/vita/gxm/gxm_texture.h"
    "${SRC_DIR}/vita/gxm/gxm_vertex.cpp"
    "${SRC_DIR}/vita/gxm/gxm_vertex.h"
    "${SRC_DIR}/vita/gxm/gxm_video.cpp"
    "${SRC_DIR}/vita/gxm/gxm_video.h"
)

set(VITA_INPUT
    "${SRC_DIR}/vita/input/vita_input.cpp"
    "${SRC_DIR}/vita/input/vita_input.h"
)

# maketree.c defines main(), which collides with newlib's crt0
set(VITA_ZLIB ${ZLIB})
list(REMOVE_ITEM VITA_ZLIB "${DEPS_DIR}/zlib/maketree.c")
