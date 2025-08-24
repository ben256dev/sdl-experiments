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

FetchContent_Declare(json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(json)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlgpu3.cpp
)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC SDL3::SDL3)

find_program(GLSLANG_VALIDATOR NAMES glslangValidator)
if(NOT GLSLANG_VALIDATOR)
    set(GLSLANG_VALIDATOR "")
endif()

set(SHADER_SRC_DIR ${CMAKE_SOURCE_DIR}/shaders)
set(SHADER_BIN_DIR shaders)

FetchContent_Declare(
    blake3
    GIT_REPOSITORY https://github.com/BLAKE3-team/BLAKE3.git
    GIT_TAG 1.5.0
)
FetchContent_MakeAvailable(blake3)

add_library(blake3 STATIC
    ${blake3_SOURCE_DIR}/c/blake3.c
    ${blake3_SOURCE_DIR}/c/blake3_dispatch.c
    ${blake3_SOURCE_DIR}/c/blake3_portable.c
)
target_include_directories(blake3 PUBLIC ${blake3_SOURCE_DIR}/c)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|i.86")
    target_sources(blake3 PRIVATE
        ${blake3_SOURCE_DIR}/c/blake3_sse2.c
        ${blake3_SOURCE_DIR}/c/blake3_sse41.c
        ${blake3_SOURCE_DIR}/c/blake3_avx2.c
        ${blake3_SOURCE_DIR}/c/blake3_avx512.c
    )
    set_source_files_properties(${blake3_SOURCE_DIR}/c/blake3_sse2.c PROPERTIES COMPILE_OPTIONS "-msse2")
    set_source_files_properties(${blake3_SOURCE_DIR}/c/blake3_sse41.c PROPERTIES COMPILE_OPTIONS "-msse4.1")
    set_source_files_properties(${blake3_SOURCE_DIR}/c/blake3_avx2.c PROPERTIES COMPILE_OPTIONS "-mavx2")
    set_source_files_properties(${blake3_SOURCE_DIR}/c/blake3_avx512.c PROPERTIES COMPILE_OPTIONS "-mavx512f;-mavx512vl")
endif()

if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|armv7|ARM")
    target_sources(blake3 PRIVATE ${blake3_SOURCE_DIR}/c/blake3_neon.c)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "armv7")
        set_source_files_properties(${blake3_SOURCE_DIR}/c/blake3_neon.c PROPERTIES COMPILE_OPTIONS "-mfpu=neon")
    endif()
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(SHADERC REQUIRED shaderc)
pkg_check_modules(GLSLANG QUIET glslang)
pkg_check_modules(SPIRV_TOOLS QUIET SPIRV-Tools)
pkg_check_modules(SPIRV_TOOLS_OPT QUIET SPIRV-Tools-opt)

set(EXTRA_SHADERC_LIBS "")
if(SHADERC_LIBRARIES MATCHES "shaderc_combined")
    if(GLSLANG_FOUND)
        list(APPEND EXTRA_SHADERC_LIBS ${GLSLANG_LIBRARIES})
    endif()
    if(SPIRV_TOOLS_FOUND)
        list(APPEND EXTRA_SHADERC_LIBS ${SPIRV_TOOLS_LIBRARIES})
    endif()
    if(SPIRV_TOOLS_OPT_FOUND)
        list(APPEND EXTRA_SHADERC_LIBS ${SPIRV_TOOLS_OPT_LIBRARIES})
    endif()
endif()

FetchContent_Declare(
    spirv_reflect
    GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Reflect.git
    GIT_TAG main
)
FetchContent_MakeAvailable(spirv_reflect)

set(SPIRV_REFLECT_TARGET "")
if(TARGET spirv-reflect-static)
    set(SPIRV_REFLECT_TARGET spirv-reflect-static)
elseif(TARGET SPIRV-Reflect)
    set(SPIRV_REFLECT_TARGET SPIRV-Reflect)
endif()
if(NOT SPIRV_REFLECT_TARGET)
    add_library(spirv_reflect_c STATIC ${spirv_reflect_SOURCE_DIR}/spirv_reflect.c)
    target_include_directories(spirv_reflect_c PUBLIC ${spirv_reflect_SOURCE_DIR})
    set(SPIRV_REFLECT_TARGET spirv_reflect_c)
endif()

add_executable(sdlgpu_imgui_triangle
    src/main.cpp
    src/shader_reflect.cpp
    src/pipeline_config.cpp
)
target_include_directories(sdlgpu_imgui_triangle PRIVATE
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${SHADERC_INCLUDE_DIRS}
    ${GLSLANG_INCLUDE_DIRS}
    ${SPIRV_TOOLS_INCLUDE_DIRS}
    ${SPIRV_TOOLS_OPT_INCLUDE_DIRS}
    ${spirv_reflect_SOURCE_DIR}
)
target_link_libraries(sdlgpu_imgui_triangle PRIVATE
    SDL3::SDL3
    imgui
    glm::glm
    blake3
    ${SHADERC_LIBRARIES}
    ${EXTRA_SHADERC_LIBS}
    ${SPIRV_REFLECT_TARGET}
    nlohmann_json::nlohmann_json
)
target_compile_definitions(sdlgpu_imgui_triangle PRIVATE
    SHADER_SRC_DIR="${SHADER_SRC_DIR}"
    SHADER_BIN_DIR="${SHADER_BIN_DIR}"
    GLSLANG_VALIDATOR_PATH="${GLSLANG_VALIDATOR}"
)

