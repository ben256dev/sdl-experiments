#include "imgui_sdl3gpu.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "imgui.h"

static unsigned char* read_all_file(const char* path, size_t* out_size) {
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

struct ImGuiSDL3GPU {
    SDL_GPUShader* vshader;
    SDL_GPUShader* fshader;
    SDL_GPUGraphicsPipeline* pipeline;
    SDL_GPUBuffer* vbo;
    SDL_GPUBuffer* ibo;
    Uint32 vbo_size;
    Uint32 ibo_size;
    SDL_GPUTexture* font_tex;
    SDL_GPUSampler* font_sampler;
};

static SDL_GPUTexture* create_font_texture(SDL_GPUDevice* device, SDL_GPUSampler** out_sampler) {
    ImGuiIO& io = ImGui::GetIO();
    unsigned char* pixels = NULL;
    int w = 0;
    int h = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    SDL_GPUTextureCreateInfo tci;
    SDL_zero(tci);
    tci.type = SDL_GPU_TEXTURETYPE_2D;
    tci.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    tci.width = (Uint32)w;
    tci.height = (Uint32)h;
    tci.layer_count_or_depth = 1;
    tci.num_levels = 1;
    tci.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    SDL_GPUTexture* tex = SDL_CreateGPUTexture(device, &tci);
    SDL_GPUTransferBufferCreateInfo tbci;
    SDL_zero(tbci);
    tbci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbci.size = (Uint32)(w * h * 4);
    SDL_GPUTransferBuffer* tbo = SDL_CreateGPUTransferBuffer(device, &tbci);
    void* map = SDL_MapGPUTransferBuffer(device, tbo, false);
    memcpy(map, pixels, (size_t)tbci.size);
    SDL_UnmapGPUTransferBuffer(device, tbo);
    SDL_GPUCommandBuffer* cb = SDL_AcquireGPUCommandBuffer(device);
    SDL_GPUCopyPass* pass = SDL_BeginGPUCopyPass(cb);
    SDL_GPUTextureTransferInfo dst;
    SDL_zero(dst);
    dst.texture = tex;
    dst.miplevel = 0;
    dst.layer = 0;
    SDL_GPUTransferBufferLocation src;
    SDL_zero(src);
    src.transfer_buffer = tbo;
    src.offset = 0;
    SDL_UploadToGPUTexture(pass, &src, 0, &dst, tci.width, tci.height, true);
    SDL_EndGPUCopyPass(pass);
    SDL_SubmitGPUCommandBuffer(cb);
    SDL_ReleaseGPUTransferBuffer(device, tbo);
    SDL_GPUSamplerCreateInfo sci;
    SDL_zero(sci);
    sci.mag_filter = SDL_GPU_FILTER_LINEAR;
    sci.min_filter = SDL_GPU_FILTER_LINEAR;
    sci.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sci.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sci.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sci.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    SDL_GPUSampler* smp = SDL_CreateGPUSampler(device, &sci);
    if (out_sampler) *out_sampler = smp;
    return tex;
}

ImGuiSDL3GPU* ImGuiSDL3GPU_Create(SDL_GPUDevice* device, SDL_Window* window, const char* vs_spv_path, const char* fs_spv_path) {
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.BackendPlatformName = "imgui_impl_sdl3gpu_platform";
    io.BackendRendererName = "imgui_impl_sdl3gpu_renderer";
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    int w, h;
    SDL_GetWindowSize(window, &w, &h);
    io.DisplaySize = ImVec2((float)w, (float)h);
    ImGuiSDL3GPU* ctx = (ImGuiSDL3GPU*)SDL_calloc(1, sizeof(ImGuiSDL3GPU));
    size_t vs_size = 0;
    size_t fs_size = 0;
    unsigned char* vs_code = read_all_file(vs_spv_path, &vs_size);
    unsigned char* fs_code = read_all_file(fs_spv_path, &fs_size);
    SDL_GPUShaderCreateInfo vsi;
    SDL_zero(vsi);
    vsi.code = vs_code;
    vsi.code_size = (Uint32)vs_size;
    vsi.format = SDL_GPU_SHADERFORMAT_SPIRV;
    vsi.stage = SDL_GPU_SHADERSTAGE_VERTEX;
    vsi.entrypoint = "main";
    ctx->vshader = SDL_CreateGPUShader(device, &vsi);
    SDL_GPUShaderCreateInfo fsi;
    SDL_zero(fsi);
    fsi.code = fs_code;
    fsi.code_size = (Uint32)fs_size;
    fsi.format = SDL_GPU_SHADERFORMAT_SPIRV;
    fsi.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    fsi.entrypoint = "main";
    ctx->fshader = SDL_CreateGPUShader(device, &fsi);
    free(vs_code);
    free(fs_code);
    SDL_GPUVertexAttribute attrs[3];
    SDL_zeroa(attrs);
    attrs[0].location = 0;
    attrs[0].buffer_slot = 0;
    attrs[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
    attrs[0].offset = 0;
    attrs[1].location = 1;
    attrs[1].buffer_slot = 0;
    attrs[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
    attrs[1].offset = sizeof(float) * 2;
    attrs[2].location = 2;
    attrs[2].buffer_slot = 0;
    attrs[2].format = SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM;
    attrs[2].offset = sizeof(float) * 4;
    SDL_GPUVertexBufferDescription vbd;
    SDL_zero(vbd);
    vbd.slot = 0;
    vbd.pitch = (Uint32)(sizeof(float) * 4 + sizeof(Uint32));
    vbd.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    SDL_GPUVertexInputState vis;
    SDL_zero(vis);
    vis.vertex_buffer_descriptions = &vbd;
    vis.num_vertex_buffers = 1;
    vis.vertex_attributes = attrs;
    vis.num_vertex_attributes = 3;
    SDL_GPUColorTargetBlendState blend;
    SDL_zero(blend);
    blend.enable_blend = true;
    blend.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    blend.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    blend.color_blendop = SDL_GPU_BLENDOP_ADD;
    blend.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    blend.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    blend.alpha_blendop = SDL_GPU_BLENDOP_ADD;
    ctx->font_tex = create_font_texture(device, &ctx->font_sampler);
    ctx->vbo_size = 0;
    ctx->ibo_size = 0;
    ctx->vbo = NULL;
    ctx->ibo = NULL;
    ctx->pipeline = NULL;
    return ctx;
}

void ImGuiSDL3GPU_Destroy(ImGuiSDL3GPU* ctx, SDL_GPUDevice* device) {
    if (!ctx) return;
    if (ctx->pipeline) SDL_ReleaseGPUGraphicsPipeline(device, ctx->pipeline);
    if (ctx->vshader) SDL_ReleaseGPUShader(device, ctx->vshader);
    if (ctx->fshader) SDL_ReleaseGPUShader(device, ctx->fshader);
    if (ctx->vbo) SDL_ReleaseGPUBuffer(device, ctx->vbo);
    if (ctx->ibo) SDL_ReleaseGPUBuffer(device, ctx->ibo);
    if (ctx->font_sampler) SDL_ReleaseGPUSampler(device, ctx->font_sampler);
    if (ctx->font_tex) SDL_ReleaseGPUTexture(device, ctx->font_tex);
    ImGui::DestroyContext();
    SDL_free(ctx);
}

void ImGuiSDL3GPU_NewFrame(ImGuiSDL3GPU* ctx, SDL_Window* window, float dt) {
    ImGuiIO& io = ImGui::GetIO();
    int w, h;
    SDL_GetWindowSize(window, &w, &h);
    io.DisplaySize = ImVec2((float)w, (float)h);
    io.DeltaTime = dt > 0 ? dt : (1.0f / 60.0f);
    ImGui::NewFrame();
}

void ImGuiSDL3GPU_Render(ImGuiSDL3GPU* ctx, SDL_GPUCommandBuffer* cb, SDL_GPUTexture* color_target, SDL_GPUTextureFormat color_format) {
    ImGui::Render();
    ImDrawData* dd = ImGui::GetDrawData();
    if (!dd || dd->TotalVtxCount == 0) return;
    Uint32 vsize = (Uint32)(dd->TotalVtxCount * (sizeof(float) * 4 + sizeof(Uint32)));
    Uint32 isize = (Uint32)(dd->TotalIdxCount * sizeof(ImDrawIdx));
    if (!ctx->vbo || ctx->vbo_size < vsize) {
        if (ctx->vbo) SDL_ReleaseGPUBuffer(SDL_GetGPUDevice(cb), ctx->vbo);
        SDL_GPUBufferCreateInfo ci;
        SDL_zero(ci);
        ci.usage = SDL_GPU_BUFFERUSAGE_VERTEX | SDL_GPU_BUFFERUSAGE_DYNAMIC;
        ci.size = vsize;
        ctx->vbo = SDL_CreateGPUBuffer(SDL_GetGPUDevice(cb), &ci);
        ctx->vbo_size = vsize;
    }
    if (!ctx->ibo || ctx->ibo_size < isize) {
        if (ctx->ibo) SDL_ReleaseGPUBuffer(SDL_GetGPUDevice(cb), ctx->ibo);
        SDL_GPUBufferCreateInfo ci;
        SDL_zero(ci);
        ci.usage = SDL_GPU_BUFFERUSAGE_INDEX | SDL_GPU_BUFFERUSAGE_DYNAMIC;
        ci.size = isize;
        ctx->ibo = SDL_CreateGPUBuffer(SDL_GetGPUDevice(cb), &ci);
        ctx->ibo_size = isize;
    }
    SDL_GPUTransferBufferCreateInfo tci_v;
    SDL_zero(tci_v);
    tci_v.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tci_v.size = vsize;
    SDL_GPUTransferBuffer* tbo_v = SDL_CreateGPUTransferBuffer(SDL_GetGPUDevice(cb), &tci_v);
    SDL_GPUTransferBufferCreateInfo tci_i;
    SDL_zero(tci_i);
    tci_i.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tci_i.size = isize;
    SDL_GPUTransferBuffer* tbo_i = SDL_CreateGPUTransferBuffer(SDL_GetGPUDevice(cb), &tci_i);
    unsigned char* vmap = (unsigned char*)SDL_MapGPUTransferBuffer(SDL_GetGPUDevice(cb), tbo_v, false);
    unsigned char* imap = (unsigned char*)SDL_MapGPUTransferBuffer(SDL_GetGPUDevice(cb), tbo_i, false);
    ImDrawVert* vtx_write = (ImDrawVert*)vmap;
    ImDrawIdx* idx_write = (ImDrawIdx*)imap;
    for (int n = 0; n < dd->CmdListsCount; n++) {
        const ImDrawList* cl = dd->CmdLists[n];
        memcpy(vtx_write, cl->VtxBuffer.Data, cl->VtxBuffer.Size * sizeof(ImDrawVert));
        memcpy(idx_write, cl->IdxBuffer.Data, cl->IdxBuffer.Size * sizeof(ImDrawIdx));
        vtx_write += cl->VtxBuffer.Size;
        idx_write += cl->IdxBuffer.Size;
    }
    SDL_UnmapGPUTransferBuffer(SDL_GetGPUDevice(cb), tbo_v);
    SDL_UnmapGPUTransferBuffer(SDL_GetGPUDevice(cb), tbo_i);
    SDL_GPUCopyPass* cpass = SDL_BeginGPUCopyPass(cb);
    SDL_GPUTransferBufferLocation src_v;
    SDL_zero(src_v);
    src_v.transfer_buffer = tbo_v;
    src_v.offset = 0;
    SDL_GPUBufferRegion dst_v;
    SDL_zero(dst_v);
    dst_v.buffer = ctx->vbo;
    dst_v.offset = 0;
    dst_v.size = vsize;
    SDL_UploadToGPUBuffer(cpass, &src_v, &dst_v, true);
    SDL_GPUTransferBufferLocation src_i;
    SDL_zero(src_i);
    src_i.transfer_buffer = tbo_i;
    src_i.offset = 0;
    SDL_GPUBufferRegion dst_i;
    SDL_zero(dst_i);
    dst_i.buffer = ctx->ibo;
    dst_i.offset = 0;
    dst_i.size = isize;
    SDL_UploadToGPUBuffer(cpass, &src_i, &dst_i, true);
    SDL_EndGPUCopyPass(cpass);
    SDL_ReleaseGPUTransferBuffer(SDL_GetGPUDevice(cb), tbo_v);
    SDL_ReleaseGPUTransferBuffer(SDL_GetGPUDevice(cb), tbo_i);
    if (!ctx->pipeline) {
        SDL_GPUColorTargetBlendState blend;
        SDL_zero(blend);
        blend.enable_blend = true;
        blend.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        blend.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        blend.color_blendop = SDL_GPU_BLENDOP_ADD;
        blend.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        blend.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        blend.alpha_blendop = SDL_GPU_BLENDOP_ADD;
        SDL_GPUColorTargetDescription cdesc;
        SDL_zero(cdesc);
        cdesc.format = color_format;
        cdesc.blend_state = blend;
        SDL_GPUGraphicsPipelineTargetInfo tgt;
        SDL_zero(tgt);
        tgt.color_target_descriptions = &cdesc;
        tgt.num_color_targets = 1;
        tgt.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_INVALID;
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
        SDL_GPUVertexAttribute attrs[3];
        SDL_zeroa(attrs);
        attrs[0].location = 0;
        attrs[0].buffer_slot = 0;
        attrs[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
        attrs[0].offset = 0;
        attrs[1].location = 1;
        attrs[1].buffer_slot = 0;
        attrs[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
        attrs[1].offset = sizeof(float) * 2;
        attrs[2].location = 2;
        attrs[2].buffer_slot = 0;
        attrs[2].format = SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM;
        attrs[2].offset = sizeof(float) * 4;
        SDL_GPUVertexBufferDescription vbd;
        SDL_zero(vbd);
        vbd.slot = 0;
        vbd.pitch = (Uint32)(sizeof(float) * 4 + sizeof(Uint32));
        vbd.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
        SDL_GPUVertexInputState vis;
        SDL_zero(vis);
        vis.vertex_buffer_descriptions = &vbd;
        vis.num_vertex_buffers = 1;
        vis.vertex_attributes = attrs;
        vis.num_vertex_attributes = 3;
        SDL_GPUGraphicsPipelineCreateInfo pci;
        SDL_zero(pci);
        pci.vertex_shader = ctx->vshader;
        pci.fragment_shader = ctx->fshader;
        pci.target_info = tgt;
        pci.vertex_input_state = vis;
        pci.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pci.rasterizer_state = rast;
        pci.depth_stencil_state = ds;
        pci.multisample_state = ms;
        ctx->pipeline = SDL_CreateGPUGraphicsPipeline(SDL_GetGPUDevice(cb), &pci);
    }
    SDL_GPUColorTargetInfo ct;
    SDL_zero(ct);
    ct.texture = color_target;
    ct.load_op = SDL_GPU_LOADOP_LOAD;
    ct.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(cb, &ct, 1, NULL);
    SDL_BindGPUGraphicsPipeline(rp, ctx->pipeline);
    SDL_GPUBindFragmentSamplers(rp, 0, &ctx->font_sampler, 1);
    SDL_GPUBindFragmentTextures(rp, 0, &ctx->font_tex, 1);
    float L = dd->DisplayPos.x;
    float R = dd->DisplayPos.x + dd->DisplaySize.x;
    float T = dd->DisplayPos.y;
    float B = dd->DisplayPos.y + dd->DisplaySize.y;
    float proj[16] = {
        2.0f/(R-L), 0, 0, 0,
        0, 2.0f/(T-B), 0, 0,
        0, 0, 1, 0,
        (R+L)/(L-R), (T+B)/(B-T), 0, 1
    };
    SDL_PushGPUVertexUniformData(cb, 0, proj, sizeof(proj));
    SDL_GPUBufferBinding vbind;
    vbind.buffer = ctx->vbo;
    vbind.offset = 0;
    SDL_BindGPUVertexBuffers(rp, 0, &vbind, 1);
    SDL_BindGPUIndexBuffer(rp, ctx->ibo, SDL_GPU_INDEXELEMENTSIZE_16BIT, 0);
    int vtx_offset = 0;
    int idx_offset = 0;
    for (int n = 0; n < dd->CmdListsCount; n++) {
        const ImDrawList* cl = dd->CmdLists[n];
        for (int cmd_i = 0; cmd_i < cl->CmdBuffer.Size; cmd_i++) {
            const ImDrawCmd* pcmd = &cl->CmdBuffer[cmd_i];
            SDL_Rect sc;
            sc.x = (int)(pcmd->ClipRect.x);
            sc.y = (int)(pcmd->ClipRect.y);
            sc.w = (int)(pcmd->ClipRect.z - pcmd->ClipRect.x);
            sc.h = (int)(pcmd->ClipRect.w - pcmd->ClipRect.y);
            SDL_SetGPUScissor(rp, &sc);
            SDL_DrawGPUIndexedPrimitives(rp, pcmd->ElemCount, 1, idx_offset, vtx_offset, 0);
            idx_offset += pcmd->ElemCount;
        }
        vtx_offset += cl->VtxBuffer.Size;
    }
    SDL_EndGPURenderPass(rp);
}

