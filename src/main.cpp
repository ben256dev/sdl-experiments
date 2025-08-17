#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include "imgui.h"
#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlgpu3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cstdarg>
#include <string>
#include <vector>
#include <filesystem>

using std::string;
namespace fs = std::filesystem;

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
    fs::path p = fs::path(a) / fs::path(b);
    return p.string();
}

static bool file_newer(const fs::path& a, const fs::path& b) {
    if (!fs::exists(a)) return false;
    if (!fs::exists(b)) return true;
    return fs::last_write_time(a) > fs::last_write_time(b);
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
    fs::create_directories(fs::path(dst).parent_path());
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
    if (file_newer(src, dst) || !fs::exists(dst)) {
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

    out->v = vshader;
    out->f = fshader;
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

int main(int, char**) {
    if (!SDL_Init(SDL_INIT_VIDEO)) return 1;

    SDL_Window* window = SDL_CreateWindow("SDL3 GPU + ImGui (docking + hot reload)", 1280, 720, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window) return 1;

    SDL_GPUDevice* device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, false, NULL);
    if (!device) return 1;
    if (!SDL_ClaimWindowForGPUDevice(device, window)) return 1;

    string exe_dir = get_exe_dir();
    if (!ensure_spv_current(exe_dir, "triangle.vert", "triangle.vert.spv")) return 1;
    if (!ensure_spv_current(exe_dir, "triangle.frag", "triangle.frag.spv")) return 1;

    size_t vs_size = 0, fs_size = 0;
    unsigned char* vs_code = read_exe_relative(exe_dir, "shaders/triangle.vert.spv", &vs_size);
    unsigned char* fs_code = read_exe_relative(exe_dir, "shaders/triangle.frag.spv", &fs_size);
    if (!vs_code || !fs_code) return 1;

    float vertices[] = {
        -0.5f, -0.5f, 1.0f, 0.2f, 0.2f,
         0.5f, -0.5f, 0.2f, 1.0f, 0.2f,
         0.0f,  0.5f, 0.2f, 0.2f, 1.0f
    };

    SDL_GPUBufferCreateInfo vb_ci;
    SDL_zero(vb_ci);
    vb_ci.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    vb_ci.size = sizeof(vertices);
    SDL_GPUBuffer* vbo = SDL_CreateGPUBuffer(device, &vb_ci);
    if (!vbo) return 1;

    SDL_GPUTransferBufferCreateInfo tb_ci;
    SDL_zero(tb_ci);
    tb_ci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tb_ci.size = sizeof(vertices);
    SDL_GPUTransferBuffer* tbo = SDL_CreateGPUTransferBuffer(device, &tb_ci);
    if (!tbo) return 1;

    void* map_ptr = SDL_MapGPUTransferBuffer(device, tbo, false);
    if (!map_ptr) return 1;
    SDL_memcpy(map_ptr, vertices, sizeof(vertices));
    SDL_UnmapGPUTransferBuffer(device, tbo);
    SDL_GPUCommandBuffer* copy_cb = SDL_AcquireGPUCommandBuffer(device);
    SDL_GPUCopyPass* copy_pass = SDL_BeginGPUCopyPass(copy_cb);
    SDL_GPUTransferBufferLocation src_loc;
    src_loc.transfer_buffer = tbo;
    src_loc.offset = 0;
    SDL_GPUBufferRegion dst_reg;
    dst_reg.buffer = vbo;
    dst_reg.offset = 0;
    dst_reg.size = sizeof(vertices);
    SDL_UploadToGPUBuffer(copy_pass, &src_loc, &dst_reg, true);
    SDL_EndGPUCopyPass(copy_pass);
    SDL_SubmitGPUCommandBuffer(copy_cb);

    SDL_GPUTextureFormat swap_fmt = SDL_GetGPUSwapchainTextureFormat(device, window);

    GpuPipeline pipe_cur = {};
    if (!build_pipeline(device, swap_fmt, &pipe_cur, vs_code, vs_size, fs_code, fs_size)) return 1;

    SDL_GPUBufferBinding vb_bind;
    vb_bind.buffer = vbo;
    vb_bind.offset = 0;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    ImGui_ImplSDL3_InitForSDLGPU(window);
    ImGui_ImplSDLGPU3_InitInfo ii;
    SDL_zero(ii);
    ii.Device = device;
    ii.ColorTargetFormat = swap_fmt;
    ii.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
    if (!ImGui_ImplSDLGPU3_Init(&ii)) return 1;

    fs::path vert_src = fs::path(SHADER_SRC_DIR) / "triangle.vert";
    fs::path frag_src = fs::path(SHADER_SRC_DIR) / "triangle.frag";
    auto v_time = fs::exists(vert_src) ? fs::last_write_time(vert_src) : fs::file_time_type::min();
    auto f_time = fs::exists(frag_src) ? fs::last_write_time(frag_src) : fs::file_time_type::min();
    Uint64 last_check = SDL_GetTicks();

    int running = 1;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT) running = 0;
            if (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) running = 0;
        }

        Uint64 now = SDL_GetTicks();
        if (now - last_check > 250) {
            last_check = now;
            bool v_changed = fs::exists(vert_src) && fs::last_write_time(vert_src) != v_time;
            bool f_changed = fs::exists(frag_src) && fs::last_write_time(frag_src) != f_time;
            if (v_changed || f_changed) {
                v_time = fs::exists(vert_src) ? fs::last_write_time(vert_src) : v_time;
                f_time = fs::exists(frag_src) ? fs::last_write_time(frag_src) : f_time;
                logf("file change detected");
                bool okv = ensure_spv_current(exe_dir, "triangle.vert", "triangle.vert.spv");
                bool okf = ensure_spv_current(exe_dir, "triangle.frag", "triangle.frag.spv");
                if (okv && okf) {
                    size_t nvs = 0, nfs = 0;
                    unsigned char* nvsb = read_exe_relative(exe_dir, "shaders/triangle.vert.spv", &nvs);
                    unsigned char* nfsb = read_exe_relative(exe_dir, "shaders/triangle.frag.spv", &nfs);
                    if (nvsb && nfsb) {
                        GpuPipeline pipe_new = {};
                        if (build_pipeline(device, swap_fmt, &pipe_new, nvsb, nvs, nfsb, nfs)) {
                            destroy_pipeline(device, &pipe_cur);
                            pipe_cur = pipe_new;
                            free(vs_code);
                            free(fs_code);
                            vs_code = nvsb;
                            fs_code = nfsb;
                            logf("pipeline reloaded");
                        } else {
                            if (nvsb) free(nvsb);
                            if (nfsb) free(nfsb);
                            logf("pipeline rebuild failed");
                        }
                    } else {
                        if (nvsb) free(nvsb);
                        if (nfsb) free(nfsb);
                        logf("failed reading rebuilt spv");
                    }
                } else {
                    logf("shader compile error");
                }
            }
        }

        ImGui_ImplSDLGPU3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(), ImGuiDockNodeFlags_PassthruCentralNode);
        ImGui::Begin("Reload Log");
        if (ImGui::Button("Clear")) g_log.clear();
        ImGui::SameLine();
        ImGui::Checkbox("Auto-scroll", &g_autoscroll);
        ImGui::Separator();
        for (auto& s : g_log) ImGui::TextUnformatted(s.c_str());
        if (g_autoscroll) ImGui::SetScrollHereY(1.0f);
        ImGui::End();
        ImGui::Render();

        SDL_GPUCommandBuffer* cb = SDL_AcquireGPUCommandBuffer(device);

        SDL_GPUTexture* swap_tex = NULL;
        Uint32 w = 0, h = 0;
        SDL_AcquireGPUSwapchainTexture(cb, window, &swap_tex, &w, &h);
        if (swap_tex) {
            SDL_GPUColorTargetInfo ct;
            SDL_zero(ct);
            ct.texture = swap_tex;
            ct.mip_level = 0;
            ct.layer_or_depth_plane = 0;
            ct.load_op = SDL_GPU_LOADOP_CLEAR;
            ct.store_op = SDL_GPU_STOREOP_STORE;
            ct.clear_color = SDL_FColor{0.05f, 0.05f, 0.08f, 1.0f};

            SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(cb, &ct, 1, NULL);
            SDL_BindGPUGraphicsPipeline(rp, pipe_cur.p);
            SDL_GPUBufferBinding vb_bind;
            vb_bind.buffer = vbo;
            vb_bind.offset = 0;
            SDL_BindGPUVertexBuffers(rp, 0, &vb_bind, 1);
            SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);

            ImGui_ImplSDLGPU3_PrepareDrawData(ImGui::GetDrawData(), cb);
            ImGui_ImplSDLGPU3_RenderDrawData(ImGui::GetDrawData(), cb, rp, NULL);

            SDL_EndGPURenderPass(rp);
        }

        SDL_SubmitGPUCommandBuffer(cb);
        SDL_Delay(1);
    }

    ImGui_ImplSDLGPU3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    destroy_pipeline(device, &pipe_cur);
    SDL_ReleaseGPUTransferBuffer(device, tbo);
    SDL_ReleaseGPUBuffer(device, vbo);
    free(vs_code);
    free(fs_code);

    SDL_ReleaseWindowFromGPUDevice(device, window);
    SDL_DestroyWindow(window);
    SDL_DestroyGPUDevice(device);
    SDL_Quit();
    return 0;
}

