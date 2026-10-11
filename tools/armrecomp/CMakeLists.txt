# armrecomp — the toolkit CLI. No external dependencies.

add_executable(armrecomp
    main.c
    container.c
    inflate.c
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
target_link_libraries(armrecomp PRIVATE vitashader)
if(NOT MSVC)
    target_link_libraries(armrecomp PRIVATE m)
endif()
