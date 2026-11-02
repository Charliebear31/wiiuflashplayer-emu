# PC test host (llvm-mingw x64 + SDL2 + OpenGL). Built by tools/build-pc.ps1.
cmake_minimum_required(VERSION 3.20)
project(rwpc C)

set(HOST ${CMAKE_CURRENT_LIST_DIR}/..)
set(GEN ${HOST}/../gen)
file(GLOB GEN_SOURCES ${GEN}/flashplayer_*.c)

# The generated code is huge; -O1 keeps compile times sane while still being fast.
set_source_files_properties(${GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "-O1;-w")

add_executable(rwpc
    ${GEN_SOURCES}
    ${HOST}/wasm-rt/wasm-rt-impl.c
    ${HOST}/wasm-rt/wasm-rt-mem-impl.c
    ${HOST}/wasm-rt/wasm-rt-exceptions-impl.c
    ${HOST}/rw_core.c
    ${HOST}/perlin.c
    ${HOST}/gfx_gl.c
    ${HOST}/main_pc.c)
target_include_directories(rwpc PRIVATE ${HOST} ${HOST}/wasm-rt ${GEN} ${SDL2_DIR}/include ${SDL2_DIR}/include/SDL2)
target_compile_definitions(rwpc PRIVATE WASM_RT_USE_MMAP=0 WASM_RT_MEMCHECK_BOUNDS_CHECK=1)
target_compile_options(rwpc PRIVATE -O2)
target_link_libraries(rwpc PRIVATE ${SDL2_DIR}/lib/libSDL2.dll.a opengl32)
