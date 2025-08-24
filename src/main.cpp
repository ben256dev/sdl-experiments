#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include "imgui.h"
#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlgpu3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <array>
#include <fstream>
#include <iterator>
#include <shaderc/shaderc.hpp>
#include "blake3.h"
#include "shader.h"
#include "shader_reflect.h"
#include "pipeline_config.h"

static inline const char* sdl_basename(const char* p)
{
    const char* b = p;
    for (const char* s = p; *s; ++s) if (*s == '/' || *s == '\\') b = s + 1;
    return b;
}

static inline void sdl_log_at(const char* what, const char* file, int line)
{
    std::fprintf(stderr, "\x1b[31mSDL: %s (%s:%d): %s\x1b[0m\n", what ? what : "", file, line, SDL_GetError());
}

static inline void sdl_die_at(const char* what, const char* file, int line)
{
    sdl_log_at(what, file, line);
    std::exit(EXIT_FAILURE);
}

#define SCRY(what) sdl_log_at((what), sdl_basename(__FILE__), __LINE__)
#define SDIE(what) sdl_die_at((what), sdl_basename(__FILE__), __LINE__)

static inline void die_at(const char* what, const char* file, int line)
{
    std::fprintf(stderr, "\x1b[31mFATAL: %s (%s:%d)\x1b[0m\n",
                 what ? what : "", file, line);
    std::exit(EXIT_FAILURE);
}

#define DIE(what) die_at((what), sdl_basename(__FILE__), __LINE__)

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

    struct frame {
        SDL_GPURenderPass*    render_pass_ptr;
        SDL_GPUCommandBuffer* command_buffer_ptr;
    };

    struct renderer {
        SDL_Window*           window_ptr;
        SDL_GPUDevice*        device_ptr;
        SDL_GPUTextureFormat  swap_format;
        SDL_GPUTexture*       msaa_color;
        SDL_GPUSampleCount    msaa;
        SDL_GPUSampleCount    imgui_msaa;
        brender::frame        frame;
    };

    static void recreate_msaa_color(brender::renderer& render)
    {
        if (render.msaa_color) {
            SDL_ReleaseGPUTexture(render.device_ptr, render.msaa_color);
            render.msaa_color = nullptr;
        }
        if (render.msaa <= SDL_GPU_SAMPLECOUNT_1) return;
        int pxw = 0, pxh = 0;
        SDL_GetWindowSizeInPixels(render.window_ptr, &pxw, &pxh);
        SDL_GPUTextureCreateInfo ci{};
        ci.type = SDL_GPU_TEXTURETYPE_2D;
        ci.format = render.swap_format;
        ci.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
        ci.width = (Uint32)pxw;
        ci.height = (Uint32)pxh;
        ci.layer_count_or_depth = 1;
        ci.num_levels = 1;
        ci.sample_count = render.msaa;
        ci.props = 0;
        render.msaa_color = SDL_CreateGPUTexture(render.device_ptr, &ci);
        if (!render.msaa_color) SDIE("SDL_CreateGPUTexture(msaa_color)");
    }

    static void imgui_backend_shutdown()
    {
        ImGui_ImplSDLGPU3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
    }

    static void imgui_backend_init(const brender::renderer& renderer)
    {
        ImGui_ImplSDL3_InitForSDLGPU(renderer.window_ptr);
        ImGui_ImplSDLGPU3_InitInfo ii;
        SDL_zero(ii);
        ii.Device = renderer.device_ptr;
        ii.ColorTargetFormat = renderer.swap_format;
        ii.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
        if (ImGui_ImplSDLGPU3_Init(&ii) == false)
            SDIE("ImGui_ImplSDLGPU3_Init()");
    }

    void imgui_xinit(brender::renderer& renderer)
    {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
        ImGui::StyleColorsDark();
        imgui_backend_init(renderer);
        renderer.imgui_msaa = SDL_GPU_SAMPLECOUNT_1;
    }

    static void imgui_reinit_if_needed(brender::renderer& renderer)
    {
        if (renderer.imgui_msaa == SDL_GPU_SAMPLECOUNT_1) return;
        ImGui_ImplSDLGPU3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui_ImplSDL3_InitForSDLGPU(renderer.window_ptr);
        ImGui_ImplSDLGPU3_InitInfo ii;
        SDL_zero(ii);
        ii.Device = renderer.device_ptr;
        ii.ColorTargetFormat = renderer.swap_format;
        ii.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
        if (ImGui_ImplSDLGPU3_Init(&ii) == false) SDIE("ImGui_ImplSDLGPU3_Init()");
        renderer.imgui_msaa = SDL_GPU_SAMPLECOUNT_1;
    }

    void xinit(brender::renderer& renderer, const brender::create_info& create_info)
    {
        if (SDL_Init(SDL_INIT_VIDEO) == false)
            SDIE("SDL_Init()");

        const brender::window& window = create_info.window;
        renderer.window_ptr = SDL_CreateWindow(window.title, window.w, window.h, window.flags);
        if (renderer.window_ptr == nullptr)
            SDIE("SDL_CreateWindow()");

        const brender::device& device = create_info.device;
        renderer.device_ptr = SDL_CreateGPUDevice(device.format_flags, device.debug_mode, device.name);
        if (renderer.device_ptr == nullptr)
            SDIE("SDL_CreateGPUDevice()");

        if (SDL_ClaimWindowForGPUDevice(renderer.device_ptr, renderer.window_ptr) == false)
            SDIE("SDL_ClaimWindowForGPUDevice()");

        renderer.swap_format = SDL_GetGPUSwapchainTextureFormat(renderer.device_ptr, renderer.window_ptr);
        if (renderer.swap_format == SDL_GPU_TEXTUREFORMAT_INVALID)
            SDIE("SDL_GetGPUSwapchainTextureFormat()");

        renderer.msaa = SDL_GPU_SAMPLECOUNT_8;
        renderer.imgui_msaa = SDL_GPU_SAMPLECOUNT_1;
        renderer.msaa_color = nullptr;
        recreate_msaa_color(renderer);

        imgui_xinit(renderer);
    }

    void imgui_render()
    {
        ImGui_ImplSDL3_NewFrame();
        ImGui_ImplSDLGPU3_NewFrame();
        ImGui::NewFrame();
        ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(), ImGuiDockNodeFlags_PassthruCentralNode);
        ImGui::Begin("Reload Log");
        if (ImGui::Button("Clear")) shader::g_log.clear();
        ImGui::SameLine();
        ImGui::Checkbox("Auto-scroll", &shader::g_autoscroll);
        ImGui::Separator();
        for (auto& s : shader::g_log) ImGui::TextUnformatted(s.c_str());
        if (shader::g_autoscroll) ImGui::SetScrollHereY(1.0f);
        ImGui::End();
        static bool show_demo = true;
        ImGui::ShowDemoWindow(&show_demo);
        ImGui::Render();
    }

    void imgui_submit(brender::frame& frame)
    {
        ImGui_ImplSDLGPU3_PrepareDrawData(ImGui::GetDrawData(), frame.command_buffer_ptr);
        ImGui_ImplSDLGPU3_RenderDrawData(ImGui::GetDrawData(), frame.command_buffer_ptr, frame.render_pass_ptr, NULL);
    }

    using draw_func_ptr = void(*)(const void*);

    void draw(brender::renderer& renderer, brender::draw_func_ptr fn, const void* data)
    {
        brender::imgui_render();

        brender::frame& frame = renderer.frame;
        frame.command_buffer_ptr = SDL_AcquireGPUCommandBuffer(renderer.device_ptr);

        SDL_GPUTexture* swap_tex = NULL;
        SDL_AcquireGPUSwapchainTexture(frame.command_buffer_ptr, renderer.window_ptr, &swap_tex, NULL, NULL);
        if (swap_tex)
        {
            if (renderer.msaa > SDL_GPU_SAMPLECOUNT_1) {
                SDL_GPUColorTargetInfo ct1;
                SDL_zero(ct1);
                ct1.texture = renderer.msaa_color;
                ct1.mip_level = 0;
                ct1.layer_or_depth_plane = 0;
                ct1.load_op = SDL_GPU_LOADOP_CLEAR;
                ct1.store_op = SDL_GPU_STOREOP_RESOLVE;
                ct1.clear_color = SDL_FColor{0.2f, 0.3f, 0.3f, 1.0f};
                ct1.resolve_texture = swap_tex;
                ct1.resolve_mip_level = 0;
                ct1.resolve_layer = 0;

                frame.render_pass_ptr = SDL_BeginGPURenderPass(frame.command_buffer_ptr, &ct1, 1, NULL);

                if (fn && data)
                    fn(data);

                SDL_EndGPURenderPass(frame.render_pass_ptr);

                SDL_GPUColorTargetInfo ct2;
                SDL_zero(ct2);
                ct2.texture = swap_tex;
                ct2.mip_level = 0;
                ct2.layer_or_depth_plane = 0;
                ct2.load_op = SDL_GPU_LOADOP_LOAD;
                ct2.store_op = SDL_GPU_STOREOP_STORE;

                frame.render_pass_ptr = SDL_BeginGPURenderPass(frame.command_buffer_ptr, &ct2, 1, NULL);

                brender::imgui_submit(renderer.frame);

                SDL_EndGPURenderPass(frame.render_pass_ptr);
            } else {
                SDL_GPUColorTargetInfo ct;
                SDL_zero(ct);
                ct.texture = swap_tex;
                ct.mip_level = 0;
                ct.layer_or_depth_plane = 0;
                ct.load_op = SDL_GPU_LOADOP_CLEAR;
                ct.store_op = SDL_GPU_STOREOP_STORE;
                ct.clear_color = SDL_FColor{0.2f, 0.3f, 0.3f, 1.0f};

                frame.render_pass_ptr = SDL_BeginGPURenderPass(frame.command_buffer_ptr, &ct, 1, NULL);

                if (fn && data)
                    fn(data);

                brender::imgui_submit(renderer.frame);

                SDL_EndGPURenderPass(frame.render_pass_ptr);
            }
        }

        SDL_SubmitGPUCommandBuffer(frame.command_buffer_ptr);

        if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
        }

        SDL_Delay(1);
    }

}

namespace shader {

    enum class type {
        vertex,
        fragment
    };

    struct file {
        type shader_type;
        std::string name;
        std::string path;
        std::string source;
        std::vector<uint32_t> spirv;
        std::array<uint8_t, BLAKE3_OUT_LEN> dgst;
    };

    struct program {
        file* vertex_shader;
        file* fragment_shader;
    };

    struct manager {
        shaderc::Compiler compiler;
        shaderc::CompileOptions opts;
        std::vector<file> files;
        std::vector<program> programs;
    };

    void init(shader::manager& manager)
    {
        manager.opts.SetOptimizationLevel(shaderc_optimization_level_performance);
        manager.opts.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_2);
    }

    void load(shader::file& shader_file, const char* filename = nullptr)
    {
        if (filename)
        {
            shader_file.name = filename;

            std::string fname(filename);
            if (fname.size() >= 5 && fname.rfind(".vert") == fname.size() - 5)
                shader_file.shader_type = type::vertex;
            else if (fname.size() >= 5 && fname.rfind(".frag") == fname.size() - 5)
                shader_file.shader_type = type::fragment;
            else
                DIE("Unknown shader extension");

            shader_file.path = std::string(SHADER_SRC_DIR) + "/" + filename;
        }

        std::ifstream fs(shader_file.path, std::ios::binary);
        if (!fs) DIE("Shader file not found");

        shader_file.source.assign(
            (std::istreambuf_iterator<char>(fs)),
            std::istreambuf_iterator<char>()
        );
        if (shader_file.source.empty()) DIE("Shader source is empty");

        blake3_hasher hasher;
        blake3_hasher_init(&hasher);
        blake3_hasher_update(&hasher, shader_file.source.data(), shader_file.source.size());
        blake3_hasher_finalize(&hasher, shader_file.dgst.data(), shader_file.dgst.size());
    }

    void compile(shader::manager& mgr, shader::file& file)
    {
        if (file.source.empty()) {
            DIE(("Tried to compile empty shader: " + file.name).c_str());
        }

        auto fres = mgr.compiler.CompileGlslToSpv(
            file.source,
            (file.shader_type == shader::type::fragment)
                ? shaderc_fragment_shader
                : shaderc_vertex_shader,
            file.name.c_str(),
            mgr.opts
        );

        if (fres.GetCompilationStatus() != shaderc_compilation_status_success) {
            DIE(fres.GetErrorMessage().c_str());
        }

        file.spirv.assign(fres.cbegin(), fres.cend());

        if (file.spirv.empty()) {
            DIE(("Shader compiled to empty SPIR-V: " + file.name).c_str());
        }
    }
}

struct draw_function_data {
    brender::frame& frame;
    shader::GpuPipeline& pipeline;
    SDL_GPUBuffer* vbo;
};

void draw_function(const void* data)
{
    const auto& d = *static_cast<const draw_function_data*>(data);
    SDL_BindGPUGraphicsPipeline(d.frame.render_pass_ptr, d.pipeline.p);
    SDL_GPUBufferBinding vb_bind;
    vb_bind.buffer = d.vbo;
    vb_bind.offset = 0;
    SDL_BindGPUVertexBuffers(d.frame.render_pass_ptr, 0, &vb_bind, 1);
    SDL_DrawGPUPrimitives(d.frame.render_pass_ptr, 3, 1, 0, 0);
}

static shaderc_optimization_level map_opt_level(const std::string& s) {
    if (s == "zero" || s == "0") return shaderc_optimization_level_zero;
    if (s == "size") return shaderc_optimization_level_size;
    if (s == "performance" || s == "p") return shaderc_optimization_level_performance;
    const char* e = std::getenv("SHADERC_OPT");
    if (e) {
        std::string ev(e);
        if (ev == "0" || ev == "zero") return shaderc_optimization_level_zero;
        if (ev == "size") return shaderc_optimization_level_size;
        if (ev == "performance" || ev == "p") return shaderc_optimization_level_performance;
    }
    return shaderc_optimization_level_performance;
}

int main(int argc, char* argv[])
{
    brender::renderer renderer;
    brender::create_info brender_info;
    brender::xinit(renderer, brender_info);

    float vertices[] = {
        -0.5f, -0.5f, 1.0f, 0.2f, 0.2f,
         0.5f, -0.5f, 0.2f, 1.0f, 0.2f,
         0.0f,  0.5f, 0.2f, 0.2f, 1.0f
    };

    SDL_GPUBufferCreateInfo vb_ci;
    SDL_zero(vb_ci);
    vb_ci.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    vb_ci.size = sizeof(vertices);
    SDL_GPUBuffer* vbo = SDL_CreateGPUBuffer(renderer.device_ptr, &vb_ci);
    if (!vbo) return 1;

    SDL_GPUTransferBufferCreateInfo tb_ci;
    SDL_zero(tb_ci);
    tb_ci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tb_ci.size = sizeof(vertices);
    SDL_GPUTransferBuffer* tbo = SDL_CreateGPUTransferBuffer(renderer.device_ptr, &tb_ci);
    if (!tbo) return 1;

    void* map_ptr = SDL_MapGPUTransferBuffer(renderer.device_ptr, tbo, false);
    if (!map_ptr) return 1;
    SDL_memcpy(map_ptr, vertices, sizeof(vertices));
    SDL_UnmapGPUTransferBuffer(renderer.device_ptr, tbo);
    SDL_GPUCommandBuffer* copy_cb = SDL_AcquireGPUCommandBuffer(renderer.device_ptr);
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

    SDL_GPUBufferBinding vb_bind;
    vb_bind.buffer = vbo;
    vb_bind.offset = 0;

    shader::manager shader_manager;
    shader::init(shader_manager);

    shader_manager.files.reserve(2);
    shader::file& vert = shader_manager.files.emplace_back();
    shader::load(vert, "triangle.vert");
    shader::file& frag = shader_manager.files.emplace_back();
    shader::load(frag, "triangle.frag");

    std::string cfg_path = std::string(SHADER_SRC_DIR) + "/" + vert.name + "+" + frag.name + ".pipeline.json";
    PipelineConfig cfg{};
    load_pipeline_config(cfg_path, cfg, 1);

    std::string global_opt = cfg.shaderc_optimization;
    std::string vs_opt = cfg.shaderc_optimization_vs.size() ? cfg.shaderc_optimization_vs : global_opt;
    std::string fs_opt = cfg.shaderc_optimization_fs.size() ? cfg.shaderc_optimization_fs : global_opt;
    if (vs_opt.empty()) vs_opt = "performance";
    if (fs_opt.empty()) fs_opt = "performance";
    shaderc_optimization_level vs_level = map_opt_level(vs_opt);
    shaderc_optimization_level fs_level = map_opt_level(fs_opt);

    shader_manager.opts.SetOptimizationLevel(vs_level);
    shader::compile(shader_manager, vert);
    shader_manager.opts.SetOptimizationLevel(fs_level);
    shader::compile(shader_manager, frag);

    shader::program& shader_program = shader_manager.programs.emplace_back();
    shader_program.vertex_shader = &vert;
    shader_program.fragment_shader = &frag;

    ReflectedVertexInput vin_ref{};
    if (!reflect_vertex_input(shader_program.vertex_shader->spirv, vin_ref)) DIE("reflect_vertex_input");
    pack_tight(vin_ref);

    ReflectedResources vres{}, fres{};
    if (!reflect_resources(shader_program.vertex_shader->spirv, vres)) DIE("reflect_resources_vs");
    if (!reflect_resources(shader_program.fragment_shader->spirv, fres)) DIE("reflect_resources_fs");

    SDL_GPUSampleCount requested = map_samples(cfg.sample_count);
    SDL_GPUSampleCount actual = choose_supported(renderer.device_ptr, renderer.swap_format, requested);
    if (renderer.msaa != actual) {
        renderer.msaa = actual;
        brender::recreate_msaa_color(renderer);
        brender::imgui_reinit_if_needed(renderer);
    }

    SDL_GPUShaderCreateInfo vci{};
    vci.code_size = shader_program.vertex_shader->spirv.size() * sizeof(uint32_t);
    vci.code = reinterpret_cast<const Uint8*>(shader_program.vertex_shader->spirv.data());
    vci.entrypoint = "main";
    vci.format = SDL_GPU_SHADERFORMAT_SPIRV;
    vci.stage = SDL_GPU_SHADERSTAGE_VERTEX;
    vci.num_samplers = vres.num_samplers;
    vci.num_storage_textures = vres.num_storage_textures;
    vci.num_storage_buffers = vres.num_storage_buffers;
    vci.num_uniform_buffers = vres.num_uniform_buffers;
    vci.props = 0;

    SDL_GPUShaderCreateInfo fci{};
    fci.code_size = shader_program.fragment_shader->spirv.size() * sizeof(uint32_t);
    fci.code = reinterpret_cast<const Uint8*>(shader_program.fragment_shader->spirv.data());
    fci.entrypoint = "main";
    fci.format = SDL_GPU_SHADERFORMAT_SPIRV;
    fci.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    fci.num_samplers = fres.num_samplers;
    fci.num_storage_textures = fres.num_storage_textures;
    fci.num_storage_buffers = fres.num_storage_buffers;
    fci.num_uniform_buffers = fres.num_uniform_buffers;
    fci.props = 0;

    SDL_GPUShader* vshader = SDL_CreateGPUShader(renderer.device_ptr, &vci);
    if (!vshader) SDIE("SDL_CreateGPUShader(vertex)");
    SDL_GPUShader* fshader = SDL_CreateGPUShader(renderer.device_ptr, &fci);
    if (!fshader) SDIE("SDL_CreateGPUShader(fragment)");

    SDL_GPUVertexInputState vin{};
    vin.vertex_buffer_descriptions = &vin_ref.buffer_desc;
    vin.num_vertex_buffers = 1;
    vin.vertex_attributes = vin_ref.attributes.data();
    vin.num_vertex_attributes = (Uint32)vin_ref.attributes.size();

    SDL_GPURasterizerState rs{};
    rs.fill_mode = SDL_GPU_FILLMODE_FILL;
    rs.cull_mode = cfg.cull;
    rs.front_face = cfg.front_face;
    rs.enable_depth_bias = false;
    rs.enable_depth_clip = true;

    SDL_GPUMultisampleState ms{};
    ms.sample_count = renderer.msaa;
    ms.sample_mask = 0;
    ms.enable_mask = false;
    ms.enable_alpha_to_coverage = false;

    SDL_GPUDepthStencilState ds{};
    ds.enable_depth_test = cfg.depth.enable;
    ds.enable_depth_write = cfg.depth.write;
    ds.enable_stencil_test = false;
    ds.compare_op = cfg.depth.compare;
    ds.compare_mask = 0xFF;
    ds.write_mask = 0xFF;

    std::vector<SDL_GPUColorTargetDescription> cdescs;
    cdescs.resize(cfg.blends.empty() ? 1 : cfg.blends.size());
    for (size_t i = 0; i < cdescs.size(); i++) {
        SDL_GPUColorTargetBlendState b{};
        if (!cfg.blends.empty()) {
            b.enable_blend = cfg.blends[i].enable;
            b.enable_color_write_mask = true;
            b.color_write_mask = cfg.blends[i].write_mask;
            b.src_color_blendfactor = cfg.blends[i].src_color;
            b.dst_color_blendfactor = cfg.blends[i].dst_color;
            b.color_blend_op = cfg.blends[i].color_op;
            b.src_alpha_blendfactor = cfg.blends[i].src_alpha;
            b.dst_alpha_blendfactor = cfg.blends[i].dst_alpha;
            b.alpha_blend_op = cfg.blends[i].alpha_op;
        } else {
            b.enable_blend = false;
            b.enable_color_write_mask = true;
            b.color_write_mask = SDL_GPU_COLORCOMPONENT_R | SDL_GPU_COLORCOMPONENT_G | SDL_GPU_COLORCOMPONENT_B | SDL_GPU_COLORCOMPONENT_A;
            b.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
            b.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
            b.color_blend_op = SDL_GPU_BLENDOP_ADD;
            b.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
            b.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
            b.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
        }
        cdescs[i].format = renderer.swap_format;
        cdescs[i].blend_state = b;
    }

    SDL_GPUGraphicsPipelineTargetInfo ti{};
    ti.color_target_descriptions = cdescs.data();
    ti.num_color_targets = (Uint32)cdescs.size();
    ti.depth_stencil_format = cfg.depth.enable ? cfg.depth.format : SDL_GPU_TEXTUREFORMAT_INVALID;
    ti.has_depth_stencil_target = cfg.depth.enable;

    SDL_GPUGraphicsPipelineCreateInfo pci{};
    pci.vertex_shader = vshader;
    pci.fragment_shader = fshader;
    pci.vertex_input_state = vin;
    pci.primitive_type = cfg.primitive;
    pci.rasterizer_state = rs;
    pci.multisample_state = ms;
    pci.depth_stencil_state = ds;
    pci.target_info = ti;
    pci.props = 0;

    SDL_GPUGraphicsPipeline* pipeline = SDL_CreateGPUGraphicsPipeline(renderer.device_ptr, &pci);
    if (!pipeline) SDIE("SDL_CreateGPUGraphicsPipeline");

    shader::GpuPipeline pipe_cur{};
    pipe_cur.p = pipeline;
    pipe_cur.v = vshader;
    pipe_cur.f = fshader;

    int running = 1;
    while (running)
    {
        SDL_Event e;
        while (SDL_PollEvent(&e))
        {
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT) running = 0;
            if (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) running = 0;
            if (e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) brender::recreate_msaa_color(renderer);
        }

        draw_function_data draw_data{ renderer.frame, pipe_cur, vbo };
        brender::draw(renderer, &draw_function, &draw_data);
    }

    brender::imgui_backend_shutdown();
    ImGui::DestroyContext();

    shader::destroy_pipeline(renderer.device_ptr, &pipe_cur);
    SDL_ReleaseGPUTransferBuffer(renderer.device_ptr, tbo);
    SDL_ReleaseGPUBuffer(renderer.device_ptr, vbo);
    if (renderer.msaa_color) SDL_ReleaseGPUTexture(renderer.device_ptr, renderer.msaa_color);

    SDL_ReleaseWindowFromGPUDevice(renderer.device_ptr, renderer.window_ptr);
    SDL_DestroyWindow(renderer.window_ptr);
    SDL_DestroyGPUDevice(renderer.device_ptr);
    SDL_Quit();
    return 0;
}

