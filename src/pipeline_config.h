#pragma once
#include <vector>
#include <string>
#include <SDL3/SDL_gpu.h>

struct JsonDepth {
    bool enable = false;
    SDL_GPUTextureFormat format = SDL_GPU_TEXTUREFORMAT_INVALID;
    bool write = false;
    SDL_GPUCompareOp compare = SDL_GPU_COMPAREOP_ALWAYS;
};

struct PipelineConfig {
    bool vertex_layout_auto = true;
    std::string vertex_shader;
    std::string fragment_shader;
    std::string entry_vs;
    std::string entry_fs;

    Uint32 sample_count;
    std::string shaderc_optimization;
    std::string shaderc_optimization_vs;
    std::string shaderc_optimization_fs;

    SDL_GPUPrimitiveType primitive;
    SDL_GPUCullMode cull;
    SDL_GPUFrontFace front_face;

    struct Depth {
        bool enable;
        bool write;
        SDL_GPUCompareOp compare;
        SDL_GPUTextureFormat format;
    } depth;

    struct Blend {
        bool enable = false;
        Uint32 write_mask = SDL_GPU_COLORCOMPONENT_R | SDL_GPU_COLORCOMPONENT_G | SDL_GPU_COLORCOMPONENT_B | SDL_GPU_COLORCOMPONENT_A;
        SDL_GPUBlendFactor src_color = SDL_GPU_BLENDFACTOR_ONE;
        SDL_GPUBlendFactor dst_color = SDL_GPU_BLENDFACTOR_ZERO;
        SDL_GPUBlendOp color_op = SDL_GPU_BLENDOP_ADD;
        SDL_GPUBlendFactor src_alpha = SDL_GPU_BLENDFACTOR_ONE;
        SDL_GPUBlendFactor dst_alpha = SDL_GPU_BLENDFACTOR_ZERO;
        SDL_GPUBlendOp alpha_op = SDL_GPU_BLENDOP_ADD;
    };

    std::vector<Blend> blends;
};

bool load_pipeline_config(const std::string& path, PipelineConfig& out, Uint32 reflected_color_attachments);

SDL_GPUSampleCount map_samples(Uint32 n);

SDL_GPUSampleCount choose_supported(SDL_GPUDevice* dev, SDL_GPUTextureFormat fmt, SDL_GPUSampleCount desired);
