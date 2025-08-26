#include "shader_reflect.h"
#include <algorithm>
#include <spirv_reflect.h>

static SDL_GPUVertexElementFormat map_spv_to_sdl(SpvReflectFormat f) {
    switch (f) {
        case SPV_REFLECT_FORMAT_R32_SFLOAT: return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT;
        case SPV_REFLECT_FORMAT_R32G32_SFLOAT: return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
        case SPV_REFLECT_FORMAT_R32G32B32_SFLOAT: return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
        case SPV_REFLECT_FORMAT_R32G32B32A32_SFLOAT: return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
        case SPV_REFLECT_FORMAT_R32_SINT: return SDL_GPU_VERTEXELEMENTFORMAT_INT;
        case SPV_REFLECT_FORMAT_R32G32_SINT: return SDL_GPU_VERTEXELEMENTFORMAT_INT2;
        case SPV_REFLECT_FORMAT_R32G32B32_SINT: return SDL_GPU_VERTEXELEMENTFORMAT_INT3;
        case SPV_REFLECT_FORMAT_R32G32B32A32_SINT: return SDL_GPU_VERTEXELEMENTFORMAT_INT4;
        case SPV_REFLECT_FORMAT_R32_UINT: return SDL_GPU_VERTEXELEMENTFORMAT_UINT;
        case SPV_REFLECT_FORMAT_R32G32_UINT: return SDL_GPU_VERTEXELEMENTFORMAT_UINT2;
        case SPV_REFLECT_FORMAT_R32G32B32_UINT: return SDL_GPU_VERTEXELEMENTFORMAT_UINT3;
        case SPV_REFLECT_FORMAT_R32G32B32A32_UINT: return SDL_GPU_VERTEXELEMENTFORMAT_UINT4;
        default: return SDL_GPU_VERTEXELEMENTFORMAT_INVALID;
    }
}

static uint32_t sdl_fmt_size(SDL_GPUVertexElementFormat f) {
    switch (f) {
        case SDL_GPU_VERTEXELEMENTFORMAT_FLOAT:   return 4;
        case SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2:  return 8;
        case SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3:  return 12;
        case SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4:  return 16;
        case SDL_GPU_VERTEXELEMENTFORMAT_INT:     return 4;
        case SDL_GPU_VERTEXELEMENTFORMAT_INT2:    return 8;
        case SDL_GPU_VERTEXELEMENTFORMAT_INT3:    return 12;
        case SDL_GPU_VERTEXELEMENTFORMAT_INT4:    return 16;
        case SDL_GPU_VERTEXELEMENTFORMAT_UINT:    return 4;
        case SDL_GPU_VERTEXELEMENTFORMAT_UINT2:   return 8;
        case SDL_GPU_VERTEXELEMENTFORMAT_UINT3:   return 12;
        case SDL_GPU_VERTEXELEMENTFORMAT_UINT4:   return 16;
        default: return 0;
    }
}

void pack_tight(ReflectedVertexInput& out) {
    std::sort(out.attributes.begin(), out.attributes.end(),
        [](const SDL_GPUVertexAttribute& a, const SDL_GPUVertexAttribute& b) { return a.location < b.location; });
    uint32_t off = 0;
    for (auto& a : out.attributes) {
        a.buffer_slot = 0;
        a.offset = off;
        off += sdl_fmt_size(a.format);
    }
    out.buffer_desc.slot = 0;
    out.buffer_desc.pitch = off;
    out.buffer_desc.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    out.buffer_desc.instance_step_rate = 0;
}

bool reflect_vertex_input(const std::vector<uint32_t>& spirv, ReflectedVertexInput& out) {
    SpvReflectShaderModule m{};
    if (spvReflectCreateShaderModule(spirv.size() * 4, spirv.data(), &m) != SPV_REFLECT_RESULT_SUCCESS) return false;
    if (m.shader_stage != SPV_REFLECT_SHADER_STAGE_VERTEX_BIT) { spvReflectDestroyShaderModule(&m); return false; }
    uint32_t count = 0;
    spvReflectEnumerateInputVariables(&m, &count, nullptr);
    std::vector<SpvReflectInterfaceVariable*> vars(count);
    spvReflectEnumerateInputVariables(&m, &count, vars.data());
    vars.erase(std::remove_if(vars.begin(), vars.end(), [](auto* v){ return v->decoration_flags & SPV_REFLECT_DECORATION_BUILT_IN; }), vars.end());
    std::sort(vars.begin(), vars.end(), [](auto* a, auto* b){ return a->location < b->location; });
    out.attributes.clear();
    uint32_t offset = 0;
    for (auto* v : vars) {
        SDL_GPUVertexElementFormat fmt = map_spv_to_sdl(v->format);
        if (fmt == SDL_GPU_VERTEXELEMENTFORMAT_INVALID) { spvReflectDestroyShaderModule(&m); return false; }
        SDL_GPUVertexAttribute a{};
        a.location = v->location;
        a.buffer_slot = 0;
        a.format = fmt;
        a.offset = offset;
        out.attributes.push_back(a);
        offset += sdl_fmt_size(fmt);
    }
    out.buffer_desc.slot = 0;
    out.buffer_desc.pitch = offset;
    out.buffer_desc.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    out.buffer_desc.instance_step_rate = 0;
    spvReflectDestroyShaderModule(&m);
    return true;
}

bool reflect_resources(const std::vector<uint32_t>& spirv, ReflectedResources& out) {
    SpvReflectShaderModule m{};
    if (spvReflectCreateShaderModule(spirv.size() * 4, spirv.data(), &m) != SPV_REFLECT_RESULT_SUCCESS) return false;
    uint32_t bind_count = 0;
    spvReflectEnumerateDescriptorBindings(&m, &bind_count, nullptr);
    std::vector<SpvReflectDescriptorBinding*> binds(bind_count);
    spvReflectEnumerateDescriptorBindings(&m, &bind_count, binds.data());
    for (auto* b : binds) {
        switch (b->descriptor_type) {
            case SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLER:
            case SPV_REFLECT_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
            case SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
                out.num_samplers += b->count;
                break;
            case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_IMAGE:
                out.num_storage_textures += b->count;
                break;
            case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
            case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
                out.num_uniform_buffers += b->count;
                break;
            case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER:
            case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
                out.num_storage_buffers += b->count;
                break;
            default: break;
        }
    }
    uint32_t pc_count = 0;
    spvReflectEnumeratePushConstantBlocks(&m, &pc_count, nullptr);
    if (pc_count > 0) {
        std::vector<SpvReflectBlockVariable*> pcs(pc_count);
        spvReflectEnumeratePushConstantBlocks(&m, &pc_count, pcs.data());
        for (auto* pc : pcs) if (pc && pc->size > out.push_constant_size) out.push_constant_size = pc->size;
    }
    spvReflectDestroyShaderModule(&m);
    return true;
}


static SDL_GPUSampleCount map_samples(Uint32 count)
{
    switch (count)
    {
        case 8: return SDL_GPU_SAMPLECOUNT_8;
        case 4: return SDL_GPU_SAMPLECOUNT_4;
        case 2: return SDL_GPU_SAMPLECOUNT_2;
        default: return SDL_GPU_SAMPLECOUNT_1;
    }
}

static SDL_GPUSampleCount choose_supported(SDL_GPUDevice* device_ptr, SDL_GPUTextureFormat format, SDL_GPUSampleCount desired)
{
    if (SDL_GPUTextureSupportsSampleCount(device_ptr, format, desired))
    {
        return desired;
    }
    SDL_GPUSampleCount candidates[] = { SDL_GPU_SAMPLECOUNT_8, SDL_GPU_SAMPLECOUNT_4, SDL_GPU_SAMPLECOUNT_2, SDL_GPU_SAMPLECOUNT_1 };
    for (SDL_GPUSampleCount candidate : candidates)
    {
        if (candidate <= desired && SDL_GPUTextureSupportsSampleCount(device_ptr, format, candidate))
        {
            return candidate;
        }
    }
    return SDL_GPU_SAMPLECOUNT_1;
}

