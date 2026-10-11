# threads — threads that wait for each other by spinning, built with the
# vitasdk toolchain. See samples/fbdemo/CMakeLists.txt for how the toolchain
# is selected; tools/wsl-vitasdk.sh builds every sample.
#
# Freestanding like fbdemo: no crt0, no newlib, entry at _start.

cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(DEFINED ENV{VITASDK})
        set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
    else()
        message(FATAL_ERROR "set VITASDK or pass -DCMAKE_TOOLCHAIN_FILE")
    endif()
endif()

project(threads C)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "threads")
set(VITA_TITLEID  "PSVR00003")
set(VITA_VERSION  "01.00")

add_executable(threads main.c)

target_compile_options(threads PRIVATE
    -O2 -fno-tree-vectorize -ffreestanding -fno-builtin -Wall -Wextra
)
target_link_options(threads PRIVATE -nostdlib -nostartfiles)
target_link_libraries(threads
    SceDisplay_stub
    SceCtrl_stub
    SceSysmem_stub
    SceLibKernel_stub
    SceKernelThreadMgr_stub
    gcc
)

vita_create_self(eboot.bin threads)
vita_create_vpk(threads.vpk ${VITA_TITLEID} eboot.bin
    VERSION ${VITA_VERSION}
    NAME    ${VITA_APP_NAME}
)
