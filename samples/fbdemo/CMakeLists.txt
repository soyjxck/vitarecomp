# fbdemo — built with the vitasdk toolchain into a SELF (eboot.bin) and a VPK.
#
# Configure with the toolchain file, which tools/wsl-vitasdk.sh does:
#   cmake -S samples/fbdemo -B build-sample \
#         -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake
#
# The program is freestanding (-nostdlib): no crt0, no newlib. Its entry is
# _start, which vita-elf-create exports as module_start by default.

cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(DEFINED ENV{VITASDK})
        set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
    else()
        message(FATAL_ERROR "set VITASDK or pass -DCMAKE_TOOLCHAIN_FILE")
    endif()
endif()

project(fbdemo C)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "fbdemo")
set(VITA_TITLEID  "PSVR00001")
set(VITA_VERSION  "01.00")

add_executable(fbdemo main.c)

# No auto-vectorisation: GCC 12+ turns the fill loop into NEON at -O2, and
# NEON is a later milestone. This fixture is meant to translate completely.
target_compile_options(fbdemo PRIVATE
    -O2 -fno-tree-vectorize -ffreestanding -fno-builtin -Wall -Wextra
)
target_link_options(fbdemo PRIVATE -nostdlib -nostartfiles)
# One stub archive per firmware library the import table will name:
#   SceSysmem     sceKernelAllocMemBlock / GetMemBlockBase / FreeMemBlock
#   SceDisplay    sceDisplaySetFrameBuf / WaitVblankStart
#   SceCtrl       sceCtrlSetSamplingMode / PeekBufferPositive
#   SceLibKernel  sceKernelExitProcess
target_link_libraries(fbdemo
    SceDisplay_stub
    SceCtrl_stub
    SceSysmem_stub
    SceLibKernel_stub
    gcc                        # __aeabi_idiv and friends, linked in, not imported
)

vita_create_self(eboot.bin fbdemo)
vita_create_vpk(fbdemo.vpk ${VITA_TITLEID} eboot.bin
    VERSION ${VITA_VERSION}
    NAME    ${VITA_APP_NAME}
)
