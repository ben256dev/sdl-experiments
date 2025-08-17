cmake_minimum_required(VERSION 3.22)
project(sdlgpu_imgui_triangle LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(SDL3 CONFIG QUIET)
if(NOT SDL3_FOUND)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL3 REQUIRED sdl3)
    add_library(SDL3::SDL3 INTERFACE IMPORTED)
    target_include_directories(SDL3::SDL3 INTERFACE ${SDL3_INCLUDE_DIRS})
    target_link_libraries(SDL3::SDL3 INTERFACE ${SDL3_LINK_LIBRARIES})
endif()

include(FetchContent)
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.92.2
)
FetchContent_MakeAvailable(imgui)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlgpu3.cpp
)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC SDL3::SDL3)

find_program(GLSLANG_VALIDATOR NAMES glslangValidator)
if(NOT GLSLANG_VALIDATOR)
    message(FATAL_ERROR "glslangValidator not found")
endif()

set(SHADER_SRC_DIR ${CMAKE_SOURCE_DIR}/shaders)
set(GEN_SRC_DIR ${CMAKE_BINARY_DIR}/gen_shaders)
file(MAKE_DIRECTORY ${GEN_SRC_DIR})

set(VERT_SRC ${SHADER_SRC_DIR}/triangle.vert)
set(FRAG_SRC ${SHADER_SRC_DIR}/triangle.frag)
if(NOT EXISTS ${VERT_SRC} OR NOT EXISTS ${FRAG_SRC})
    set(VERT_SRC ${GEN_SRC_DIR}/triangle.vert)
    set(FRAG_SRC ${GEN_SRC_DIR}/triangle.frag)
    set(VERT_SRC_CODE [=[#version 450
layout(location = 0) in vec2 in_pos;
layout(location = 1) in vec3 in_col;
layout(location = 0) out vec3 v_col;
void main() {
    v_col = in_col;
    gl_Position = vec4(in_pos, 0.0, 1.0);
}
]=])
    set(FRAG_SRC_CODE [=[#version 450
layout(location = 0) in vec3 v_col;
layout(location = 0) out vec4 out_col;
void main() {
    out_col = vec4(v_col, 1.0);
}
]=])
    file(WRITE ${VERT_SRC} "${VERT_SRC_CODE}")
    file(WRITE ${FRAG_SRC} "${FRAG_SRC_CODE}")
endif()

set(SHADER_BIN_DIR ${CMAKE_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_BIN_DIR})
set(VERT_SPV ${SHADER_BIN_DIR}/triangle.vert.spv)
set(FRAG_SPV ${SHADER_BIN_DIR}/triangle.frag.spv)

add_custom_command(
    OUTPUT ${VERT_SPV}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_BIN_DIR}
    COMMAND ${GLSLANG_VALIDATOR} -V -o ${VERT_SPV} ${VERT_SRC}
    DEPENDS ${VERT_SRC}
)
add_custom_command(
    OUTPUT ${FRAG_SPV}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_BIN_DIR}
    COMMAND ${GLSLANG_VALIDATOR} -V -o ${FRAG_SPV} ${FRAG_SRC}
    DEPENDS ${FRAG_SRC}
)
add_custom_target(compile_shaders DEPENDS ${VERT_SPV} ${FRAG_SPV})

add_executable(sdlgpu_imgui_triangle src/main.cpp)
add_dependencies(sdlgpu_imgui_triangle compile_shaders)
target_include_directories(sdlgpu_imgui_triangle PRIVATE ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(sdlgpu_imgui_triangle PRIVATE SDL3::SDL3 imgui)

