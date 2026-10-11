# All tests are synthetic. No game data, no dump, no key material — a fresh
# clone runs the full suite with nothing installed and nothing supplied.

add_executable(test_container
    test_container.c
    ${CMAKE_SOURCE_DIR}/tools/armrecomp/container.c
)

target_include_directories(test_container PRIVATE
    ${CMAKE_SOURCE_DIR}/tools/armrecomp
)

# Release configurations define NDEBUG, which compiles every assert() in the
# suite to nothing — the tests then print "all passed" while checking nothing.
# Strip it for test targets. test_container.c also #errors if NDEBUG survives,
# so this cannot silently regress.
target_compile_options(test_container PRIVATE -UNDEBUG)

add_test(NAME container COMMAND test_container)

add_executable(test_inflate
    test_inflate.c
    ${CMAKE_SOURCE_DIR}/tools/armrecomp/inflate.c
)

target_include_directories(test_inflate PRIVATE
    ${CMAKE_SOURCE_DIR}/tools/armrecomp
)

target_compile_options(test_inflate PRIVATE -UNDEBUG)

add_test(NAME inflate COMMAND test_inflate)

add_executable(test_decode
    test_decode.c
    ${CMAKE_SOURCE_DIR}/tools/armrecomp/decode.c
)

target_include_directories(test_decode PRIVATE
    ${CMAKE_SOURCE_DIR}/tools/armrecomp
)

target_compile_options(test_decode PRIVATE -UNDEBUG)

add_test(NAME decode COMMAND test_decode)

add_executable(test_reloc
    test_reloc.c
    ${CMAKE_SOURCE_DIR}/tools/armrecomp/reloc.c
)

target_include_directories(test_reloc PRIVATE
    ${CMAKE_SOURCE_DIR}/tools/armrecomp
)

target_compile_options(test_reloc PRIVATE -UNDEBUG)

add_test(NAME reloc COMMAND test_reloc)

add_executable(test_runtime test_runtime.c)
target_link_libraries(test_runtime PRIVATE vitarecomp)
target_compile_options(test_runtime PRIVATE -UNDEBUG)
add_test(NAME runtime COMMAND test_runtime)

add_executable(test_raster test_raster.c ${CMAKE_SOURCE_DIR}/src/raster.c)
target_compile_options(test_raster PRIVATE -UNDEBUG)
if(NOT MSVC)
    target_link_libraries(test_raster PRIVATE m)
endif()
add_test(NAME raster COMMAND test_raster)

add_executable(test_texdec test_texdec.c ${CMAKE_SOURCE_DIR}/src/texdec.c ${CMAKE_SOURCE_DIR}/src/pvrtc.c)
target_compile_options(test_texdec PRIVATE -UNDEBUG)
add_test(NAME texdec COMMAND test_texdec)

add_executable(test_usse test_usse.c ${CMAKE_SOURCE_DIR}/src/usse.c)
target_include_directories(test_usse PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_compile_options(test_usse PRIVATE -UNDEBUG)
if(NOT MSVC)
    target_link_libraries(test_usse PRIVATE m)
endif()
add_test(NAME usse COMMAND test_usse)

add_executable(test_pixfmt test_pixfmt.c ${CMAKE_SOURCE_DIR}/src/pixfmt.c)
target_include_directories(test_pixfmt PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_compile_options(test_pixfmt PRIVATE -UNDEBUG)
if(NOT MSVC)
    target_link_libraries(test_pixfmt PRIVATE m)
endif()
add_test(NAME pixfmt COMMAND test_pixfmt)

add_executable(test_gxm_tex test_gxm_tex.c ${CMAKE_SOURCE_DIR}/src/gxm_tex.c ${CMAKE_SOURCE_DIR}/src/pixfmt.c
               ${CMAKE_SOURCE_DIR}/src/texdec.c ${CMAKE_SOURCE_DIR}/src/pvrtc.c)
target_include_directories(test_gxm_tex PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_compile_options(test_gxm_tex PRIVATE -UNDEBUG)
if(NOT MSVC)
    target_link_libraries(test_gxm_tex PRIVATE m)
endif()
add_test(NAME gxm_tex COMMAND test_gxm_tex)

add_executable(test_pool test_pool.c ${CMAKE_SOURCE_DIR}/src/pool.c)
target_compile_options(test_pool PRIVATE -UNDEBUG)
if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(test_pool PRIVATE Threads::Threads)
endif()
add_test(NAME pool COMMAND test_pool)
