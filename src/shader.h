#pragma once

#include <cstdarg>
#include <vector>
#include <string>
#include <filesystem>

using std::string;

namespace shader {

static std::vector<string> g_log;
static bool g_autoscroll = true;

static void logf(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_log.emplace_back(buf);
    if (g_log.size() > 2000) g_log.erase(g_log.begin(), g_log.begin() + 1000);
}

static unsigned char* read_all(const char* path, size_t* out_size) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    unsigned char* buf = (unsigned char*)malloc((size_t)n);
    if (!buf) { fclose(f); return NULL; }
    size_t r = fread(buf, 1, (size_t)n, f);
    fclose(f);
    if (r != (size_t)n) { free(buf); return NULL; }
    *out_size = (size_t)n;
    return buf;
}

static string get_exe_dir() {
    const char* b = SDL_GetBasePath();
    if (!b) return string();
    string s(b);
    SDL_free((void*)b);
    return s;
}

static string join_paths(const string& a, const string& b) {
    std::filesystem::path p = std::filesystem::path(a) / std::filesystem::path(b);
    return p.string();
}

static bool file_newer(const std::filesystem::path& a, const std::filesystem::path& b) {
    if (!std::filesystem::exists(a)) return false;
    if (!std::filesystem::exists(b)) return true;
    return std::filesystem::last_write_time(a) > std::filesystem::last_write_time(b);
}

#ifndef SHADER_SRC_DIR
#define SHADER_SRC_DIR "."
#endif
#ifndef SHADER_BIN_DIR
#define SHADER_BIN_DIR "shaders"
#endif
#ifndef GLSLANG_VALIDATOR_PATH
#define GLSLANG_VALIDATOR_PATH "glslangValidator"
#endif

static int run_cmd_capture(const string& cmd, string* out) {
    string full = cmd + " 2>&1";
    FILE* p = popen(full.c_str(), "r");
    if (!p) return -1;
    char buf[4096];
    out->clear();
    while (fgets(buf, sizeof(buf), p)) (*out) += buf;
    int rc = pclose(p);
    return rc;
}

static bool compile_glsl_to_spv(const string& src, const string& dst) {
    std::filesystem::create_directories(std::filesystem::path(dst).parent_path());
    string cmd = string("\"") + GLSLANG_VALIDATOR_PATH + "\" -V -o \"" + dst + "\" \"" + src + "\"";
    string out;
    int rc = run_cmd_capture(cmd, &out);
    if (rc == 0) {
        logf("compiled: %s -> %s", src.c_str(), dst.c_str());
        return true;
    } else {
        logf("compile failed: %s", src.c_str());
        if (!out.empty()) logf("%s", out.c_str());
        return false;
    }
}

static bool ensure_spv_current(const string& exe_dir, const char* glsl_name, const char* spv_name) {
    string src = join_paths(SHADER_SRC_DIR, glsl_name);
    string dst = join_paths(exe_dir + SHADER_BIN_DIR + string("/"), spv_name);
    if (file_newer(src, dst) || !std::filesystem::exists(dst)) {
        logf("rebuilding: %s", src.c_str());
        if (!compile_glsl_to_spv(src, dst)) return false;
    }
    return true;
}

static unsigned char* read_exe_relative(const string& exe_dir, const char* rel, size_t* out_size) {
    string full = join_paths(exe_dir, rel);
    unsigned char* d = read_all(full.c_str(), out_size);
    if (d) return d;
    return read_all(rel, out_size);
}

struct GpuPipeline {
    SDL_GPUShader* v = nullptr;
    SDL_GPUShader* f = nullptr;
    SDL_GPUGraphicsPipeline* p = nullptr;
};

static bool build_pipeline(SDL_GPUDevice* device, SDL_GPUTextureFormat swap_fmt, GpuPipeline* out, const unsigned char* vs, size_t vs_size, const unsigned char* fsb, size_t fs_size) {
    SDL_GPUShaderCreateInfo vs_ci;
    SDL_zero(vs_ci);
    vs_ci.code = vs;
    vs_ci.code_size = vs_size;
    vs_ci.format = SDL_GPU_SHADERFORMAT_SPIRV;
    vs_ci.stage = SDL_GPU_SHADERSTAGE_VERTEX;
    vs_ci.entrypoint = "main";
    SDL_GPUShader* vshader = SDL_CreateGPUShader(device, &vs_ci);
    if (!vshader) return false;

    SDL_GPUShaderCreateInfo fs_ci;
    SDL_zero(fs_ci);
    fs_ci.code = fsb;
    fs_ci.code_size = fs_size;
    fs_ci.format = SDL_GPU_SHADERFORMAT_SPIRV;
    fs_ci.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    fs_ci.entrypoint = "main";
    SDL_GPUShader* fshader = SDL_CreateGPUShader(device, &fs_ci);
    if (!fshader) {
        SDL_ReleaseGPUShader(device, vshader);
        return false;
    }

    SDL_GPUColorTargetBlendState blend_state;
    SDL_zero(blend_state);
    blend_state.enable_blend = false;

    SDL_GPUColorTargetDescription color_desc;
    color_desc.format = swap_fmt;
    color_desc.blend_state = blend_state;

    SDL_GPUGraphicsPipelineTargetInfo target_info;
    SDL_zero(target_info);
    target_info.color_target_descriptions = &color_desc;
    target_info.num_color_targets = 1;
    target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_INVALID;

    SDL_GPURasterizerState rast;
    SDL_zero(rast);
    rast.fill_mode = SDL_GPU_FILLMODE_FILL;
    rast.cull_mode = SDL_GPU_CULLMODE_NONE;
    rast.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;

    SDL_GPUDepthStencilState ds;
    SDL_zero(ds);
    ds.enable_depth_test = false;
    ds.enable_depth_write = false;

    SDL_GPUMultisampleState ms;
    SDL_zero(ms);
    ms.sample_count = SDL_GPU_SAMPLECOUNT_1;

    SDL_GPUVertexAttribute attrs[2];
    SDL_zeroa(attrs);
    attrs[0].location = 0;
    attrs[0].buffer_slot = 0;
    attrs[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
    attrs[0].offset = 0;
    attrs[1].location = 1;
    attrs[1].buffer_slot = 0;
    attrs[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
    attrs[1].offset = sizeof(float) * 2;

    SDL_GPUVertexBufferDescription vbd;
    SDL_zero(vbd);
    vbd.slot = 0;
    vbd.pitch = sizeof(float) * 5;
    vbd.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

    SDL_GPUVertexInputState vis;
    SDL_zero(vis);
    vis.vertex_buffer_descriptions = &vbd;
    vis.num_vertex_buffers = 1;
    vis.vertex_attributes = attrs;
    vis.num_vertex_attributes = 2;

    SDL_GPUGraphicsPipelineCreateInfo ci;
    SDL_zero(ci);
    ci.vertex_shader = vshader;
    ci.fragment_shader = fshader;
    ci.target_info = target_info;
    ci.vertex_input_state = vis;
    ci.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    ci.rasterizer_state = rast;
    ci.depth_stencil_state = ds;
    ci.multisample_state = ms;

    SDL_GPUGraphicsPipeline* pipeline = SDL_CreateGPUGraphicsPipeline(device, &ci);
    if (!pipeline) {
        SDL_ReleaseGPUShader(device, fshader);
        SDL_ReleaseGPUShader(device, vshader);
        return false;
    }

    out->p = pipeline;
    return true;
}

static void destroy_pipeline(SDL_GPUDevice* device, GpuPipeline* gp) {
    if (gp->p) SDL_ReleaseGPUGraphicsPipeline(device, gp->p);
    if (gp->f) SDL_ReleaseGPUShader(device, gp->f);
    if (gp->v) SDL_ReleaseGPUShader(device, gp->v);
    gp->p = nullptr;
    gp->f = nullptr;
    gp->v = nullptr;
}

}
