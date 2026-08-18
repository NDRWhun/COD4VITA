# Sanity check the CMakeLists.txt
if (NOT KISAK_PLATFORM STREQUAL "vita")
    message(FATAL_ERROR "KISAK_PLATFORM is incorrect for building vita.")
endif()

if (NOT DEFINED ENV{VITASDK})
    message(FATAL_ERROR "VITASDK is not set; configure with -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake")
endif()

# vita_create_self / vita_create_vpk
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

# under gnu++20 newlib declares long random(), which ambiguates com_math.h's float random()
set(CMAKE_CXX_EXTENSIONS OFF)

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O2 -g")
set(CMAKE_CXX_FLAGS_DEBUG   "${CMAKE_CXX_FLAGS_DEBUG} -O0 -g")
set(CMAKE_C_FLAGS_RELEASE   "${CMAKE_C_FLAGS_RELEASE} -O2 -g")
set(CMAKE_C_FLAGS_DEBUG     "${CMAKE_C_FLAGS_DEBUG} -O0 -g")

# kernel modules the platform, GXM and input layers call
set(VITA_STUB_LIBS
    SceGxm_stub
    SceDisplay_stub
    SceCtrl_stub
    SceTouch_stub
    SceCommonDialog_stub
    SceIme_stub
    SceKernelDmacMgr_stub
    ScePower_stub
    SceVideodec_stub
    SceAudiodec_stub
    SceSysmodule_stub
)
