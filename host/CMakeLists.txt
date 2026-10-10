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

add_executable(vita_host
    main.c
    "${VITARECOMP_RECOMP_DIR}/recomp_funcs.c"
)
target_link_libraries(vita_host PRIVATE vitarecomp)

if(VITARECOMP_IMPORT_DEFAULTS)
    add_library(recomp_imports STATIC ${VITARECOMP_IMPORT_DEFAULTS})
    target_link_libraries(recomp_imports PRIVATE vitarecomp)
    target_link_libraries(vita_host PRIVATE recomp_imports)
endif()
