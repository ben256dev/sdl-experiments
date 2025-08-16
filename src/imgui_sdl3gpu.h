#ifndef IMGUI_SDL3GPU_H
#define IMGUI_SDL3GPU_H

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ImGuiSDL3GPU ImGuiSDL3GPU;

ImGuiSDL3GPU* ImGuiSDL3GPU_Create(SDL_GPUDevice* device, SDL_Window* window, const char* vs_spv_path, const char* fs_spv_path);
void ImGuiSDL3GPU_Destroy(ImGuiSDL3GPU* ctx, SDL_GPUDevice* device);
void ImGuiSDL3GPU_NewFrame(ImGuiSDL3GPU* ctx, SDL_Window* window, float dt);
void ImGuiSDL3GPU_Render(ImGuiSDL3GPU* ctx, SDL_GPUCommandBuffer* cb, SDL_GPUTexture* color_target, SDL_GPUTextureFormat color_format);

#ifdef __cplusplus
}
#endif

#endif

