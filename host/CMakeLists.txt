# The host: what answers include/vitarecomp/host.h, as objects any program
# of recompiled code links, and the way to make such a program.
#
#   vitahost               host.c and the platform's window and sampler
#   vitarecomp_add_game()  a program from one module's output of `armrecomp
#                          emit`, the host, the runtime, and whatever sources
#                          of its own it is given
#   vita_host              the generic one, main.c and nothing of its own:
#                          built when the top level is configured with
#
#     -DVITARECOMP_RECOMP_DIR=<dir holding recomp_funcs.c and recomp_funcs_imports/>

# The window is the platform's own: GDI on Windows, none elsewhere yet.
if(WIN32)
    set(VITARECOMP_HOST_WINDOW window_win32.c)
    set(VITARECOMP_HOST_SAMPLER sampler_win32.c)
else()
    set(VITARECOMP_HOST_WINDOW window_none.c)
    set(VITARECOMP_HOST_SAMPLER sampler_none.c)
endif()

# Objects and not an archive: the runtime calls the host and the host the
# runtime, and two archives that need each other link only in an order no
# single order is.
add_library(vitahost OBJECT
    host.c
    ${VITARECOMP_HOST_WINDOW}
    ${VITARECOMP_HOST_SAMPLER}
)
target_link_libraries(vitahost PUBLIC vitarecomp)

# Names and lines for the host and the runtime in the linked program; the
# generated C stays without, there is too much of it.
if(VITARECOMP_SYMBOLS)
    if(MSVC)
        target_compile_options(vitahost PRIVATE /Z7)
    else()
        target_compile_options(vitahost PRIVATE -g)
    endif()
endif()

# vitarecomp_add_game(<target> RECOMP_DIR <dir> [SOURCES <file>...])
#
# A program of one module's translated code. The sources given are the
# program's own and hold its main(), which calls vita_host_main
# (include/vitarecomp/host_main.h); vita_host below is the smallest case.
#
# The functions arrive in parts (recomp_funcs_000.c, _001.c, ...) beside the
# main file, each including the generated recomp_funcs.h, so they compile in
# parallel. A module small enough for one part has just the one.
#
# The import defaults go into their own static library, listed AFTER the
# runtime: a linker pulls an archive member only for a symbol still undefined,
# so an import the runtime implements is never shadowed by its trapping default
# and an import it does not yet implement still links. See include/vitarecomp/hle.h.
function(vitarecomp_add_game target)
    cmake_parse_arguments(GAME "" "RECOMP_DIR" "SOURCES" ${ARGN})
    if(NOT GAME_RECOMP_DIR OR NOT EXISTS "${GAME_RECOMP_DIR}/recomp_funcs.c")
        message(FATAL_ERROR "vitarecomp_add_game(${target}): RECOMP_DIR must be a directory produced by `armrecomp emit`")
    endif()
    file(GLOB _parts "${GAME_RECOMP_DIR}/recomp_funcs_[0-9][0-9][0-9].c")
    file(GLOB _imports "${GAME_RECOMP_DIR}/recomp_funcs_imports/*.c")

    add_executable(${target}
        ${GAME_SOURCES}
        "${GAME_RECOMP_DIR}/recomp_funcs.c"
        ${_parts}
    )
    target_link_libraries(${target} PRIVATE vitahost vitarecomp)
    if(WIN32)
        target_link_libraries(${target} PRIVATE user32 gdi32 dbghelp winmm)
    endif()
    if(VITARECOMP_SYMBOLS AND MSVC)
        target_link_options(${target} PRIVATE /DEBUG /OPT:REF /OPT:ICF)
    endif()

    set(_targets ${target})
    if(_imports)
        add_library(${target}_imports STATIC ${_imports})
        target_link_libraries(${target}_imports PRIVATE vitarecomp)
        target_link_libraries(${target} PRIVATE ${target}_imports)
        list(APPEND _targets ${target}_imports)
    endif()

    # What this file's directory gives its own targets, said again: the
    # function is called from another project's directory, which has none
    # of it. The generated C is millions of lines; MSVC's default object
    # format cannot hold that many sections.
    foreach(_t IN LISTS _targets)
        set_target_properties(${_t} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
        if(MSVC)
            target_compile_options(${_t} PRIVATE /bigobj)
            target_compile_definitions(${_t} PRIVATE _CRT_SECURE_NO_WARNINGS)
        endif()
    endforeach()
endfunction()

if(VITARECOMP_RECOMP_DIR)
    vitarecomp_add_game(vita_host RECOMP_DIR "${VITARECOMP_RECOMP_DIR}" SOURCES main.c)
endif()
