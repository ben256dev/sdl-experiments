#pragma once
#include <vector>
#include <cstdint>
#include <SDL3/SDL_gpu.h>

struct ReflectedVertexInput {
    std::vector<SDL_GPUVertexAttribute> attributes;
    SDL_GPUVertexBufferDescription buffer_desc{};
};

struct ReflectedResources {
    uint32_t num_samplers = 0;
    uint32_t num_storage_textures = 0;
    uint32_t num_storage_buffers = 0;
    uint32_t num_uniform_buffers = 0;
    uint32_t push_constant_size = 0;
};

bool reflect_vertex_input(const std::vector<uint32_t>& spirv, ReflectedVertexInput& out);
bool reflect_resources(const std::vector<uint32_t>& spirv, ReflectedResources& out);

