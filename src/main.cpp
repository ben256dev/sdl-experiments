#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include "imgui.h"
#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlgpu3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shader.h"

namespace brender {
    struct window {
        const char* title = "brender";
        int w = 1280;
        int h = 720;
        Uint32 flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    };

    struct device {
        SDL_GPUShaderFormat format_flags = SDL_GPU_SHADERFORMAT_SPIRV;
        bool debug_mode = false;
        const char* name = NULL;
    };

    struct create_info {
        brender::window window;
        brender::device device;
    };

    struct context {
        SDL_Window*           window_ptr;
        SDL_GPUDevice*        device_ptr;
        SDL_GPUTextureFormat  swap_format;
    };

    void xinit(brender::context& context, const brender::create_info& create_info)
    {
        if (SDL_Init(SDL_INIT_VIDEO) == false)
            exit(EXIT_FAILURE);

        const brender::window& window = create_info.window;
        context.window_ptr = SDL_CreateWindow(window.title, window.w, window.h, window.flags);
        if (context.window_ptr == nullptr)
            exit(EXIT_FAILURE);

        const brender::device& device = create_info.device;
        context.device_ptr = SDL_CreateGPUDevice(device.format_flags, device.debug_mode, device.name);
        if (context.device_ptr == nullptr)
            exit(EXIT_FAILURE);

        if (!SDL_ClaimWindowForGPUDevice(context.device_ptr, context.window_ptr))
            exit(EXIT_FAILURE);
    }

    void imgui_init(const brender::context& context)
    {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        ImGui_ImplSDL3_InitForSDLGPU(context.window_ptr);
        ImGui_ImplSDLGPU3_InitInfo ii;
        SDL_zero(ii);
        ii.Device = context.device_ptr;
        ii.ColorTargetFormat = context.swap_format;
        ii.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
        if (!ImGui_ImplSDLGPU3_Init(&ii))
            exit(EXIT_FAILURE);
    }
}



int main(int argc, char* argv[])
{
    brender::context context;
    brender::create_info brender_info;
    brender::xinit(context, brender_info);

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
    SDL_GPUBuffer* vbo = SDL_CreateGPUBuffer(context.device_ptr, &vb_ci);
    if (!vbo) return 1;

    SDL_GPUTransferBufferCreateInfo tb_ci;
    SDL_zero(tb_ci);
    tb_ci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tb_ci.size = sizeof(vertices);
    SDL_GPUTransferBuffer* tbo = SDL_CreateGPUTransferBuffer(context.device_ptr, &tb_ci);
    if (!tbo) return 1;

    void* map_ptr = SDL_MapGPUTransferBuffer(context.device_ptr, tbo, false);
    if (!map_ptr) return 1;
    SDL_memcpy(map_ptr, vertices, sizeof(vertices));
    SDL_UnmapGPUTransferBuffer(context.device_ptr, tbo);
    SDL_GPUCommandBuffer* copy_cb = SDL_AcquireGPUCommandBuffer(context.device_ptr);
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

    context.swap_format = SDL_GetGPUSwapchainTextureFormat(context.device_ptr, context.window_ptr);

    GpuPipeline pipe_cur = {};
    if (!build_pipeline(context.device_ptr, context.swap_format, &pipe_cur, vs_code, vs_size, fs_code, fs_size)) return 1;

    SDL_GPUBufferBinding vb_bind;
    vb_bind.buffer = vbo;
    vb_bind.offset = 0;

    brender::imgui_init(context);

    std::filesystem::path vert_src = std::filesystem::path(SHADER_SRC_DIR) / "triangle.vert";
    std::filesystem::path frag_src = std::filesystem::path(SHADER_SRC_DIR) / "triangle.frag";
    auto v_time = std::filesystem::exists(vert_src) ? std::filesystem::last_write_time(vert_src) : std::filesystem::file_time_type::min();
    auto f_time = std::filesystem::exists(frag_src) ? std::filesystem::last_write_time(frag_src) : std::filesystem::file_time_type::min();
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
            bool v_changed = std::filesystem::exists(vert_src) && std::filesystem::last_write_time(vert_src) != v_time;
            bool f_changed = std::filesystem::exists(frag_src) && std::filesystem::last_write_time(frag_src) != f_time;
            if (v_changed || f_changed) {
                v_time = std::filesystem::exists(vert_src) ? std::filesystem::last_write_time(vert_src) : v_time;
                f_time = std::filesystem::exists(frag_src) ? std::filesystem::last_write_time(frag_src) : f_time;
                logf("file change detected");
                bool okv = ensure_spv_current(exe_dir, "triangle.vert", "triangle.vert.spv");
                bool okf = ensure_spv_current(exe_dir, "triangle.frag", "triangle.frag.spv");
                if (okv && okf) {
                    size_t nvs = 0, nfs = 0;
                    unsigned char* nvsb = read_exe_relative(exe_dir, "shaders/triangle.vert.spv", &nvs);
                    unsigned char* nfsb = read_exe_relative(exe_dir, "shaders/triangle.frag.spv", &nfs);
                    if (nvsb && nfsb) {
                        GpuPipeline pipe_new = {};
                        if (build_pipeline(context.device_ptr, context.swap_format, &pipe_new, nvsb, nvs, nfsb, nfs)) {
                            destroy_pipeline(context.device_ptr, &pipe_cur);
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

        SDL_GPUCommandBuffer* cb = SDL_AcquireGPUCommandBuffer(context.device_ptr);

        SDL_GPUTexture* swap_tex = NULL;
        Uint32 w = 0, h = 0;
        SDL_AcquireGPUSwapchainTexture(cb, context.window_ptr, &swap_tex, &w, &h);
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

    destroy_pipeline(context.device_ptr, &pipe_cur);
    SDL_ReleaseGPUTransferBuffer(context.device_ptr, tbo);
    SDL_ReleaseGPUBuffer(context.device_ptr, vbo);
    free(vs_code);
    free(fs_code);

    SDL_ReleaseWindowFromGPUDevice(context.device_ptr, context.window_ptr);
    SDL_DestroyWindow(context.window_ptr);
    SDL_DestroyGPUDevice(context.device_ptr);
    SDL_Quit();
    return 0;
}

