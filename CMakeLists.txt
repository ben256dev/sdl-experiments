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
    GIT_TAG docking
)
FetchContent_MakeAvailable(imgui)

FetchContent_Declare(
    glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG 1.0.1
)
FetchContent_MakeAvailable(glm)

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
set(SHADER_BIN_DIR shaders)

add_executable(sdlgpu_imgui_triangle src/main.cpp)
target_include_directories(sdlgpu_imgui_triangle PRIVATE ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(sdlgpu_imgui_triangle PRIVATE SDL3::SDL3 imgui glm::glm)
target_compile_definitions(sdlgpu_imgui_triangle PRIVATE
    SHADER_SRC_DIR="${SHADER_SRC_DIR}"
    SHADER_BIN_DIR="${SHADER_BIN_DIR}"
    GLSLANG_VALIDATOR_PATH="${GLSLANG_VALIDATOR}"
)

