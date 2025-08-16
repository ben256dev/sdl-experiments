#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include "imgui.h"
#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlgpu3.h"

int main(int, char**) {
    if (!SDL_Init(SDL_INIT_VIDEO)) return 1;

    SDL_Window* window = SDL_CreateWindow("ImGui + SDL3 GPU", 1280, 720, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window) return 1;

    SDL_GPUShaderFormat formats = (SDL_GPUShaderFormat)(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_MSL | SDL_GPU_SHADERFORMAT_DXIL);
    SDL_GPUDevice* device = SDL_CreateGPUDevice(formats, false, nullptr);
    if (!device) return 1;
    if (!SDL_ClaimWindowForGPUDevice(device, window)) return 1;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui_ImplSDL3_InitForSDLGPU(window);

    ImGui_ImplSDLGPU3_InitInfo info = {};
    info.Device = device;
    info.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(device, window);
    info.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
    if (!ImGui_ImplSDLGPU3_Init(&info)) return 1;

    bool running = true;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT) running = false;
            if (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && e.window.windowID == SDL_GetWindowID(window)) running = false;
        }
        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) {
            SDL_Delay(10);
            continue;
        }

        ImGui_ImplSDLGPU3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowSize(ImVec2(360, 200), ImGuiCond_Once);
        ImGui::Begin("Triangle");
        ImVec2 c = ImGui::GetWindowPos();
        ImVec2 s = ImGui::GetWindowSize();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p0 = ImVec2(c.x + s.x * 0.50f, c.y + s.y * 0.20f);
        ImVec2 p1 = ImVec2(c.x + s.x * 0.20f, c.y + s.y * 0.80f);
        ImVec2 p2 = ImVec2(c.x + s.x * 0.80f, c.y + s.y * 0.80f);
        dl->AddTriangleFilled(p0, p1, p2, IM_COL32(255, 64, 64, 255));
        ImGui::End();

        ImGui::Render();

        SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device);
        if (!cmd) break;

        SDL_GPUTexture* backbuffer = nullptr;
        Uint32 w = 0, h = 0;
        if (!SDL_AcquireGPUSwapchainTexture(cmd, window, &backbuffer, &w, &h) || !backbuffer) {
            SDL_SubmitGPUCommandBuffer(cmd);
            continue;
        }

        SDL_GPUColorTargetInfo ct = {};
        ct.texture = backbuffer;
        ct.mip_level = 0;
        ct.layer_or_depth_plane = 0;
        ct.load_op = SDL_GPU_LOADOP_CLEAR;
        ct.store_op = SDL_GPU_STOREOP_STORE;
        ct.clear_color = SDL_FColor{0.08f, 0.08f, 0.10f, 1.0f};

        ImGui_ImplSDLGPU3_PrepareDrawData(ImGui::GetDrawData(), cmd);

        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &ct, 1, nullptr);
        ImGui_ImplSDLGPU3_RenderDrawData(ImGui::GetDrawData(), cmd, pass, nullptr);
        SDL_EndGPURenderPass(pass);

        SDL_SubmitGPUCommandBuffer(cmd);
    }

    ImGui_ImplSDLGPU3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    SDL_ReleaseWindowFromGPUDevice(device, window);
    SDL_DestroyWindow(window);
    SDL_DestroyGPUDevice(device);
    SDL_Quit();
    return 0;
}

