# psvitarecomp — a static-recompilation toolkit for PlayStation Vita games.
#
# This file builds:
#   1. the `vitarecomp` runtime static library — what recompiled game code links
#      against (CPU state, memory, semantic helpers, the HLE)
#   2. `armrecomp` — the tool that turns a Vita module into C
#   3. optionally `vita_host`, the headless host, against one module's emitted C
#
# A game repo consumes this repo as a git submodule and does:
#     add_subdirectory(psvitarecomp)
#     add_subdirectory(games/<game>)
# and links its host against the `vitarecomp` target.
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
    src/hle_libm.c
    src/hle_ult.c
    src/hle_gxm.c
    src/hle_ngs.c
    src/hle_stubs.c
    src/hle_movie.c
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
    src/usse.c
    src/gxp.c
    src/pixfmt.c
    src/gxm_tex.c
    src/gxm_draw.c
    src/symbols.c
)

# Guest threads are host threads (sched.c), one running at a time.
if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(vitarecomp PUBLIC Threads::Threads)
endif()

target_include_directories(vitarecomp PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# --- the recompiler -----------------------------------------------------------

add_subdirectory(tools/armrecomp)

# --- the host, against one module's emitted C ---------------------------------

set(VITARECOMP_RECOMP_DIR "" CACHE PATH
    "Directory holding recomp_funcs.c and recomp_funcs_imports/ from `armrecomp emit`; builds vita_host against it")
if(VITARECOMP_RECOMP_DIR)
    add_subdirectory(host)
endif()

# --- tests (only when vitarecomp is the top-level project, not a submodule) ----

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    enable_testing()
    add_subdirectory(tests)
endif()
