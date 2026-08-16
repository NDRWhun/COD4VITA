set(VITA_PLATFORM
    "${SRC_DIR}/vita/platform/vita_errorscreen.cpp"
    "${SRC_DIR}/vita/platform/vita_errorscreen.h"
    "${SRC_DIR}/vita/platform/vita_files.cpp"
    "${SRC_DIR}/vita/platform/vita_files.h"
    "${SRC_DIR}/vita/platform/vita_globals.cpp"
    "${SRC_DIR}/vita/platform/vita_main.cpp"
    "${SRC_DIR}/vita/platform/vita_memory.cpp"
    "${SRC_DIR}/vita/platform/vita_memory.h"
    "${SRC_DIR}/vita/platform/vita_net.cpp"
    "${SRC_DIR}/vita/platform/vita_prefix.h"
    "${SRC_DIR}/vita/platform/vita_selftest.cpp"
    "${SRC_DIR}/vita/platform/vita_selftest.h"
    "${SRC_DIR}/vita/platform/vita_sys.cpp"
    "${SRC_DIR}/vita/platform/vita_sys.h"
    "${SRC_DIR}/vita/platform/vita_system.cpp"
    "${SRC_DIR}/vita/platform/vita_system.h"
    "${SRC_DIR}/vita/platform/vita_threads.cpp"
    "${SRC_DIR}/vita/platform/vita_threads.h"
)

# the Miles and OpenAL backends stay out; snd_vita/snd_driver_vita replace both halves
set(VITA_SOUND
    "${SRC_DIR}/sound/snd.cpp"
    "${SRC_DIR}/sound/snd_driver_load_obj.cpp"
    "${SRC_DIR}/sound/snd_driver_vita.cpp"
    "${SRC_DIR}/sound/snd_local.h"
    "${SRC_DIR}/sound/snd_public.h"
    "${SRC_DIR}/sound/snd_utils.cpp"
    "${SRC_DIR}/sound/snd_vita.cpp"
    "${SRC_DIR}/sound/snd_vita.h"
)

set(VITA_GXM
    "${SRC_DIR}/vita/gxm/gxm_blit.cpp"
    "${SRC_DIR}/vita/gxm/gxm_blit.h"
    "${SRC_DIR}/vita/gxm/gxm_blit_shaders.h"
    "${SRC_DIR}/vita/gxm/gxm_buffer.cpp"
    "${SRC_DIR}/vita/gxm/gxm_buffer.h"
    "${SRC_DIR}/vita/gxm/gxm_clear_shaders.h"
    "${SRC_DIR}/vita/gxm/gxm_device.cpp"
    "${SRC_DIR}/vita/gxm/gxm_device.h"
    "${SRC_DIR}/vita/gxm/gxm_draw.cpp"
    "${SRC_DIR}/vita/gxm/gxm_draw.h"
    "${SRC_DIR}/vita/gxm/gxm_fence.cpp"
    "${SRC_DIR}/vita/gxm/gxm_fence.h"
    "${SRC_DIR}/vita/gxm/gxm_image.cpp"
    "${SRC_DIR}/vita/gxm/gxm_image.h"
    "${SRC_DIR}/vita/gxm/gxm_material.cpp"
    "${SRC_DIR}/vita/gxm/gxm_material.h"
    "${SRC_DIR}/vita/gxm/gxm_memory.cpp"
    "${SRC_DIR}/vita/gxm/gxm_memory.h"
    "${SRC_DIR}/vita/gxm/gxm_pipeline.cpp"
    "${SRC_DIR}/vita/gxm/gxm_pipeline.h"
    "${SRC_DIR}/vita/gxm/gxm_program.cpp"
    "${SRC_DIR}/vita/gxm/gxm_program.h"
    "${SRC_DIR}/vita/gxm/gxm_rendertarget.cpp"
    "${SRC_DIR}/vita/gxm/gxm_rendertarget.h"
    "${SRC_DIR}/vita/gxm/gxm_scissor.cpp"
    "${SRC_DIR}/vita/gxm/gxm_scissor.h"
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
    "${SRC_DIR}/vita/input/vita_input_engine.cpp"
    "${SRC_DIR}/vita/input/vita_livestorage.cpp"
)

# maketree.c defines main(), which collides with newlib's crt0
set(VITA_ZLIB ${ZLIB})
list(REMOVE_ITEM VITA_ZLIB "${DEPS_DIR}/zlib/maketree.c")
