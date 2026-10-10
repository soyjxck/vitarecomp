# hello — a newlib-linked Vita program, built with the vitasdk toolchain.
# See samples/fbdemo/CMakeLists.txt for how the toolchain is selected;
# tools/wsl-vitasdk.sh builds every sample.

cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(DEFINED ENV{VITASDK})
        set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
    else()
        message(FATAL_ERROR "set VITASDK or pass -DCMAKE_TOOLCHAIN_FILE")
    endif()
endif()

project(hello C)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "hello")
set(VITA_TITLEID  "PSVR00002")
set(VITA_VERSION  "01.00")

add_executable(hello main.c)

# No auto-vectorisation: NEON is a later milestone.
target_compile_options(hello PRIVATE -O2 -fno-tree-vectorize -Wall -Wextra)

# The ordinary homebrew link: newlib and its runtime come from the toolchain;
# these are the firmware libraries the program and the runtime reach for.
target_link_libraries(hello
    SceDisplay_stub
    SceCtrl_stub
    SceSysmem_stub
    SceLibKernel_stub
    SceIofilemgr_stub
    SceProcessmgr_stub
    SceKernelThreadMgr_stub
)

vita_create_self(eboot.bin hello)
vita_create_vpk(hello.vpk ${VITA_TITLEID} eboot.bin
    VERSION ${VITA_VERSION}
    NAME    ${VITA_APP_NAME}
)
