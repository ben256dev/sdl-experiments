#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include <stdio.h>
#include <stdlib.h>

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

int main(int argc, char** argv) {
    if (!SDL_Init(SDL_INIT_VIDEO)) return 1;

    SDL_Window* window = SDL_CreateWindow("SDL3 GPU Triangle", 800, 600, SDL_WINDOW_RESIZABLE);
    if (!window) return 1;

    SDL_GPUDevice* device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, true, NULL);
    if (!device) return 1;

    if (!SDL_ClaimWindowForGPUDevice(device, window)) return 1;

    size_t vs_size = 0;
    size_t fs_size = 0;
    unsigned char* vs_code = read_all("shaders/triangle.vert.spv", &vs_size);
    unsigned char* fs_code = read_all("shaders/triangle.frag.spv", &fs_size);
    if (!vs_code || !fs_code) return 1;

    SDL_GPUShaderCreateInfo vs_info;
    SDL_zero(vs_info);
    vs_info.code = vs_code;
    vs_info.code_size = vs_size;
    vs_info.format = SDL_GPU_SHADERFORMAT_SPIRV;
    vs_info.stage = SDL_GPU_SHADERSTAGE_VERTEX;
    vs_info.entrypoint = "main";
    SDL_GPUShader* vshader = SDL_CreateGPUShader(device, &vs_info);
    if (!vshader) return 1;

    SDL_GPUShaderCreateInfo fs_info;
    SDL_zero(fs_info);
    fs_info.code = fs_code;
    fs_info.code_size = fs_size;
    fs_info.format = SDL_GPU_SHADERFORMAT_SPIRV;
    fs_info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    fs_info.entrypoint = "main";
    SDL_GPUShader* fshader = SDL_CreateGPUShader(device, &fs_info);
    if (!fshader) return 1;

    float vertices[] = {
        -0.5f, -0.5f, 1.0f, 0.2f, 0.2f,
         0.5f, -0.5f, 0.2f, 1.0f, 0.2f,
         0.0f,  0.5f, 0.2f, 0.2f, 1.0f
    };
    const Uint32 vertex_stride = sizeof(float) * 5;

    SDL_GPUBufferCreateInfo vb_ci;
    SDL_zero(vb_ci);
    vb_ci.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    vb_ci.size = sizeof(vertices);
    vb_ci.props = 0;
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
    vbd.pitch = vertex_stride;
    vbd.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    vbd.instance_step_rate = 0;

    SDL_GPUVertexInputState vis;
    SDL_zero(vis);
    vis.vertex_buffer_descriptions = &vbd;
    vis.num_vertex_buffers = 1;
    vis.vertex_attributes = attrs;
    vis.num_vertex_attributes = 2;

    SDL_GPUTextureFormat swap_fmt = SDL_GetGPUSwapchainTextureFormat(device, window);

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

    SDL_GPUGraphicsPipelineCreateInfo pipe_ci;
    SDL_zero(pipe_ci);
    pipe_ci.vertex_shader = vshader;
    pipe_ci.fragment_shader = fshader;
    pipe_ci.target_info = target_info;
    pipe_ci.vertex_input_state = vis;
    pipe_ci.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pipe_ci.rasterizer_state = rast;
    pipe_ci.depth_stencil_state = ds;
    pipe_ci.multisample_state = ms;

    SDL_GPUGraphicsPipeline* pipeline = SDL_CreateGPUGraphicsPipeline(device, &pipe_ci);
    if (!pipeline) return 1;

    SDL_GPUBufferBinding vb_bind;
    vb_bind.buffer = vbo;
    vb_bind.offset = 0;

    int running = 1;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) running = 0;
            if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE) running = 0;
        }

        SDL_GPUCommandBuffer* cb = SDL_AcquireGPUCommandBuffer(device);

        SDL_GPUTexture* swap_tex = NULL;
        SDL_AcquireGPUSwapchainTexture(cb, window, &swap_tex, NULL, NULL);
        if (swap_tex) {
            SDL_GPUColorTargetInfo ct;
            SDL_zero(ct);
            ct.texture = swap_tex;
            ct.load_op = SDL_GPU_LOADOP_CLEAR;
            ct.store_op = SDL_GPU_STOREOP_STORE;
            ct.clear_color.r = 0.05f;
            ct.clear_color.g = 0.05f;
            ct.clear_color.b = 0.08f;
            ct.clear_color.a = 1.0f;

            SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(cb, &ct, 1, NULL);
            SDL_BindGPUGraphicsPipeline(rp, pipeline);
            SDL_BindGPUVertexBuffers(rp, 0, &vb_bind, 1);
            SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
            SDL_EndGPURenderPass(rp);
        }

        SDL_SubmitGPUCommandBuffer(cb);
        SDL_Delay(1);
    }

    SDL_ReleaseGPUGraphicsPipeline(device, pipeline);
    SDL_ReleaseGPUShader(device, fshader);
    SDL_ReleaseGPUShader(device, vshader);
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

