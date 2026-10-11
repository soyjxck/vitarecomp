# armrecomp — the toolkit CLI. No external dependencies.

# What reads a module's container, SELF or ELF, and turns the one into the
# other: a library of its own, because a game's program does that too, to
# the player's copy, when it is set up (container.h).
add_library(vitacontainer STATIC
    container.c
    extract.c
    inflate.c
)
target_include_directories(vitacontainer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(armrecomp
    main.c
    decode.c
    module.c
    reloc.c
    analyze.c
    emit.c
    nids.c
    shader.c
)

# The shader listing shares the runtime's bytecode decoder.
target_include_directories(armrecomp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(armrecomp PRIVATE vitashader vitacontainer)
if(NOT MSVC)
    target_link_libraries(armrecomp PRIVATE m)
endif()
