cmake_minimum_required(VERSION 3.18)
project(sdl3_gpu_triangle C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(USE_SYSTEM_SDL "Use system-installed SDL3 via pkg-config" OFF)

if (USE_SYSTEM_SDL)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL3 REQUIRED sdl3)
    add_library(SDL3::SDL3 INTERFACE IMPORTED)
    target_include_directories(SDL3::SDL3 INTERFACE ${SDL3_INCLUDE_DIRS})
    target_link_directories(SDL3::SDL3 INTERFACE ${SDL3_LIBRARY_DIRS})
    target_link_libraries(SDL3::SDL3 INTERFACE ${SDL3_LIBRARIES})
else()
    include(FetchContent)
    FetchContent_Declare(
        SDL
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG main
    )
    FetchContent_MakeAvailable(SDL)
endif()

add_executable(sdl3_gpu_triangle src/main.c)

target_link_libraries(sdl3_gpu_triangle PRIVATE SDL3::SDL3)
if (UNIX)
    target_link_libraries(sdl3_gpu_triangle PRIVATE m)
endif()

find_program(GLSLANG_VALIDATOR glslangValidator)
if (NOT GLSLANG_VALIDATOR)
    message(FATAL_ERROR "glslangValidator not found")
endif()

set(SHADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
set(BUILD_SHADER_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${BUILD_SHADER_DIR})

add_custom_command(
    OUTPUT ${BUILD_SHADER_DIR}/triangle.vert.spv
    COMMAND ${GLSLANG_VALIDATOR} -V -S vert -o ${BUILD_SHADER_DIR}/triangle.vert.spv ${SHADER_DIR}/triangle.vert
    DEPENDS ${SHADER_DIR}/triangle.vert
    VERBATIM
)

add_custom_command(
    OUTPUT ${BUILD_SHADER_DIR}/triangle.frag.spv
    COMMAND ${GLSLANG_VALIDATOR} -V -S frag -o ${BUILD_SHADER_DIR}/triangle.frag.spv ${SHADER_DIR}/triangle.frag
    DEPENDS ${SHADER_DIR}/triangle.frag
    VERBATIM
)

add_custom_target(compile_shaders
    DEPENDS
        ${BUILD_SHADER_DIR}/triangle.vert.spv
        ${BUILD_SHADER_DIR}/triangle.frag.spv
)

add_dependencies(sdl3_gpu_triangle compile_shaders)

