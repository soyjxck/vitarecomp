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
    ${CMAKE_SOURCE_DIR}/src/usse.c
    ${CMAKE_SOURCE_DIR}/src/gxp.c
)

# The shader listing shares the runtime's bytecode decoder.
target_include_directories(armrecomp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
)
if(NOT MSVC)
    target_link_libraries(armrecomp PRIVATE m)
endif()
