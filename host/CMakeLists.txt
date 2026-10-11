# vita_host — the generic headless host, built against one module's output
# from `armrecomp emit`. Configure the top level with
#
#     -DVITARECOMP_RECOMP_DIR=<dir holding recomp_funcs.c and recomp_funcs_imports/>
#
# The import defaults go into their own static library, listed AFTER the
# runtime: a linker pulls an archive member only for a symbol still undefined,
# so an import the runtime implements is never shadowed by its trapping default
# and an import it does not yet implement still links. See include/vitarecomp/hle.h.

if(NOT VITARECOMP_RECOMP_DIR)
    message(FATAL_ERROR "host: set VITARECOMP_RECOMP_DIR to a directory produced by `armrecomp emit`")
endif()

file(GLOB VITARECOMP_IMPORT_DEFAULTS "${VITARECOMP_RECOMP_DIR}/recomp_funcs_imports/*.c")

# The functions arrive in parts (recomp_funcs_000.c, _001.c, ...) beside the
# main file, each including the generated recomp_funcs.h, so they compile in
# parallel. A module small enough for one part has just the one.
file(GLOB VITARECOMP_RECOMP_PARTS "${VITARECOMP_RECOMP_DIR}/recomp_funcs_[0-9][0-9][0-9].c")

# The window is the platform's own: GDI on Windows, none elsewhere yet.
if(WIN32)
    set(VITARECOMP_HOST_WINDOW window_win32.c)
    set(VITARECOMP_HOST_SAMPLER sampler_win32.c)
else()
    set(VITARECOMP_HOST_WINDOW window_none.c)
    set(VITARECOMP_HOST_SAMPLER sampler_none.c)
endif()

add_executable(vita_host
    main.c
    ${VITARECOMP_HOST_WINDOW}
    ${VITARECOMP_HOST_SAMPLER}
    "${VITARECOMP_RECOMP_DIR}/recomp_funcs.c"
    ${VITARECOMP_RECOMP_PARTS}
)
target_link_libraries(vita_host PRIVATE vitarecomp)
if(WIN32)
    target_link_libraries(vita_host PRIVATE user32 gdi32 dbghelp winmm)
endif()

# Names and lines for the host and the runtime in the linked program; the
# generated C stays without, there is too much of it.
if(VITARECOMP_SYMBOLS)
    if(MSVC)
        set_source_files_properties(main.c ${VITARECOMP_HOST_WINDOW} ${VITARECOMP_HOST_SAMPLER}
                                    PROPERTIES COMPILE_OPTIONS "/Z7")
        target_link_options(vita_host PRIVATE /DEBUG /OPT:REF /OPT:ICF)
    else()
        set_source_files_properties(main.c ${VITARECOMP_HOST_WINDOW} ${VITARECOMP_HOST_SAMPLER}
                                    PROPERTIES COMPILE_OPTIONS "-g")
    endif()
endif()

if(VITARECOMP_IMPORT_DEFAULTS)
    add_library(recomp_imports STATIC ${VITARECOMP_IMPORT_DEFAULTS})
    target_link_libraries(recomp_imports PRIVATE vitarecomp)
    target_link_libraries(vita_host PRIVATE recomp_imports)
endif()
