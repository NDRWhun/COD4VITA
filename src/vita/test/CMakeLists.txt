cmake_minimum_required(VERSION 3.10)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain")
  else()
    message(FATAL_ERROR "Set VITASDK to your VitaSDK root, or pass CMAKE_TOOLCHAIN_FILE.")
  endif()
endif()

project(gxm_smoke CXX)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "KisakCOD GXM smoke")
set(VITA_TITLEID  "KCOD00001")
set(VITA_VERSION  "01.00")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -O2 -ffast-math")

set(VITA_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

# the platform layer shares the engine's scalar type header
include_directories(${VITA_ROOT})

add_executable(gxm_smoke
  gxm_smoke.cpp
  ${VITA_ROOT}/vita/gxm/gxm_memory.cpp
  ${VITA_ROOT}/vita/gxm/gxm_device.cpp
  ${VITA_ROOT}/vita/gxm/gxm_state.cpp
  ${VITA_ROOT}/vita/gxm/gxm_program.cpp
  ${VITA_ROOT}/vita/gxm/gxm_vertex.cpp
  ${VITA_ROOT}/vita/gxm/gxm_draw.cpp
  ${VITA_ROOT}/vita/gxm/gxm_texture.cpp
  ${VITA_ROOT}/vita/gxm/gxm_buffer.cpp
  ${VITA_ROOT}/vita/gxm/gxm_video.cpp
  ${VITA_ROOT}/vita/gxm/gxm_shader_archive.cpp
  ${VITA_ROOT}/vita/input/vita_input.cpp
  ${VITA_ROOT}/vita/platform/vita_memory.cpp
  ${VITA_ROOT}/vita/platform/vita_threads.cpp
  ${VITA_ROOT}/vita/platform/vita_files.cpp
  ${VITA_ROOT}/vita/platform/vita_sys.cpp
  ${VITA_ROOT}/vita/platform/vita_selftest.cpp
)

target_link_libraries(gxm_smoke
  SceGxm_stub
  SceDisplay_stub
  SceCtrl_stub
  SceTouch_stub
  SceCommonDialog_stub
  SceIme_stub
  SceKernelDmacMgr_stub
)

vita_create_self(gxm_smoke.self gxm_smoke UNSAFE)

# the shader archive is built from game assets, so it is passed in rather than committed
if(DEFINED SHADER_ARCHIVE AND EXISTS ${SHADER_ARCHIVE})
  message(STATUS "packing shader archive: ${SHADER_ARCHIVE}")
  vita_create_vpk(gxm_smoke.vpk ${VITA_TITLEID} gxm_smoke.self
    VERSION ${VITA_VERSION}
    NAME ${VITA_APP_NAME}
    FILE ${SHADER_ARCHIVE} shaders.kgxp
  )
else()
  vita_create_vpk(gxm_smoke.vpk ${VITA_TITLEID} gxm_smoke.self
    VERSION ${VITA_VERSION}
    NAME ${VITA_APP_NAME}
  )
endif()
