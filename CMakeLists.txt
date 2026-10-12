# vitarecomp — a static-recompilation toolkit for PlayStation Vita games.
#
# This file builds:
#   1. the `vitarecomp` runtime static library — what recompiled game code links
#      against (CPU state, memory, semantic helpers, the HLE)
#   2. `armrecomp` — the tool that turns a Vita module into C
#   3. the host, as objects a program of recompiled code links, and optionally
#      `vita_host`, the generic such program, against one module's emitted C
#
# A game repo consumes this repo as a git submodule and does:
#     add_subdirectory(vitarecomp)
#     vitarecomp_add_game(<game> RECOMP_DIR <its emitted C> SOURCES <its own main and the rest>)
# which is the host with the game's own main() in front (host/CMakeLists.txt).
#
# The core has no external dependencies on purpose. SDL2, zlib and friends only
# ever appear behind an opt-in option, so a fresh clone builds with nothing
# installed. (zlib matters here in a way it did not for psprecomp: SELF segments
# are routinely zlib-compressed *as well as* encrypted. The inflate path is
# written in-tree and the container parser reports compression without needing it.)

cmake_minimum_required(VERSION 3.16)
project(vitarecomp C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/W3)
    # The generated C is millions of lines; MSVC's default object format
    # cannot hold that many sections.
    add_compile_options(/bigobj)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

# --- the runtime library ------------------------------------------------------

# The shader bytecode: its decoder, its two interpreters, the program
# container. A library of its own because the recompiler's tool lists
# programs with it too, and because one of its files is built for a newer
# processor than the rest and chosen at run time.
add_library(vitashader STATIC
    src/usse.c
    src/gxp.c
    src/gxp_hlsl.c
    src/half_rows.c
)
target_include_directories(vitashader PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|x86_64|amd64)$")
    target_sources(vitashader PRIVATE src/half_rows_avx2.c)
    target_compile_definitions(vitashader PRIVATE VITA_HALF_ROWS_AVX2)
    if(MSVC)
        set_source_files_properties(src/half_rows_avx2.c PROPERTIES COMPILE_OPTIONS "/arch:AVX2")
    else()
        set_source_files_properties(src/half_rows_avx2.c PROPERTIES COMPILE_OPTIONS "-mavx2;-mf16c")
    endif()
endif()
if(NOT MSVC)
    target_link_libraries(vitashader PUBLIC m)
endif()

add_library(vitarecomp STATIC
    src/cpu.c
    src/mem.c
    src/trap.c
    src/loader.c
    src/dispatch.c
    src/vfp.c
    src/hle_libc.c
    src/hle_sysmem.c
    src/hle_display.c
    src/hle_ctrl.c
    src/hle_kernel.c
    src/hle_threadmgr.c
    src/hle_io.c
    src/hle_clib.c
    src/hle_mspace.c
    src/hle_fios.c
    src/psarc.c
    src/hle_libm.c
    src/hle_ult.c
    src/hle_gxm.c
    src/hle_ngs.c
    src/hle_savedata.c
    src/hle_stubs.c
    src/hle_touch.c
    src/hle_movie.c
    src/hle_near.c
    src/hle_vars.c
    src/hle_scanf.c
    src/hle_rtc.c
    src/sched.c
    src/clock.c
    src/pool.c
    src/profile.c
    src/raster.c
    src/texdec.c
    src/pvrtc.c
    src/pixfmt.c
    src/gxm_tex.c
    src/gxm_draw.c
    src/gxm_gpu.c
    src/symbols.c
)

# Guest threads are host threads (sched.c), one running at a time.
if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(vitarecomp PUBLIC Threads::Threads)
endif()

target_link_libraries(vitarecomp PUBLIC vitashader)

# An archive's blocks are deflated (src/psarc.c), and the inflate that undoes
# it is the one the container reader has (tools/armrecomp/inflate.c).
target_link_libraries(vitarecomp PUBLIC vitacontainer)

# A scene's draws on the host's graphics card (src/gxm_gpu.h): Direct3D 11
# where there is one, which is part of the system and adds nothing to ship.
# Elsewhere there is no card yet and every scene is drawn on the processor.
if(WIN32)
    target_sources(vitarecomp PRIVATE src/gpu_d3d11.c)
    target_link_libraries(vitarecomp PUBLIC d3d11 d3dcompiler dxguid)
else()
    target_sources(vitarecomp PRIVATE src/gpu_none.c)
endif()

# Debug information for the runtime, and for the host when one is built: what
# a debugger or a sampling profiler needs to name a line. Off by default; a
# release build is the same code with or without it.
option(VITARECOMP_SYMBOLS "Debug information for the runtime and the host" OFF)
if(VITARECOMP_SYMBOLS)
    foreach(target vitashader vitarecomp)
        if(MSVC)
            target_compile_options(${target} PRIVATE /Z7)
        else()
            target_compile_options(${target} PRIVATE -g)
        endif()
    endforeach()
endif()

target_include_directories(vitarecomp PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# --- the recompiler -----------------------------------------------------------

add_subdirectory(tools/armrecomp)

# --- the host, and vita_host against one module's emitted C --------------------

set(VITARECOMP_RECOMP_DIR "" CACHE PATH
    "Directory holding recomp_funcs.c and recomp_funcs_imports/ from `armrecomp emit`; builds vita_host against it")
add_subdirectory(host)

# --- tests (only when vitarecomp is the top-level project, not a submodule) ----

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    enable_testing()
    add_subdirectory(tests)
endif()
