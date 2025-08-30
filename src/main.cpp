#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include "imgui.h"
#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlgpu3.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <array>
#include <fstream>
#include <iterator>
#include <memory>
#include <thread>
#include <chrono>
#include <shaderc/shaderc.hpp>
#include "blake3.h"
#include "shader.h"
#include "shader_reflect.h"
#include "pipeline_config.h"

#ifndef SHADER_SRC_DIR
#define SHADER_SRC_DIR "shaders"
#endif

static inline const char* sdl_basename(const char* program_path)
{
    const char* base = program_path;
    for (const char* scan = program_path; *scan; ++scan)
        if (*scan == '/' || *scan == '\\') base = scan + 1;
    return base;
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

enum class SceneMode { Docked, Fullscreen };
static SceneMode g_mode = SceneMode::Docked;

namespace brender
{

    struct window {
        const char* title = "brender";
        int width = 1280;
        int height = 720;
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
        SDL_Window* window_ptr = nullptr;
        SDL_GPUDevice* device_ptr = nullptr;
        SDL_GPUTextureFormat swap_format = SDL_GPU_TEXTUREFORMAT_INVALID;
        SDL_GPUTexture* msaa_color = nullptr;
        SDL_GPUSampleCount msaa = SDL_GPU_SAMPLECOUNT_1;
        SDL_GPUSampleCount imgui_msaa = SDL_GPU_SAMPLECOUNT_1;
        brender::frame frame{};
        SDL_GPUTexture* scene_msaa = nullptr;
        SDL_GPUTexture* scene_tex = nullptr;
        SDL_GPUSampler* scene_sampler = nullptr;
        SDL_GPUTextureSamplerBinding scene_binding{};
        int scene_w = 0, scene_h = 0;
    };

    static void create_target(brender::renderer& render)
    {
        if (render.msaa_color)
        {
            SDL_ReleaseGPUTexture(render.device_ptr, render.msaa_color);
            render.msaa_color = nullptr;
        }
        if (render.msaa <= SDL_GPU_SAMPLECOUNT_1)
            return;
        int pixel_width = 0;
        int pixel_height = 0;
        SDL_GetWindowSizeInPixels(render.window_ptr, &pixel_width, &pixel_height);
        SDL_GPUTextureCreateInfo texture_info{};
        texture_info.type = SDL_GPU_TEXTURETYPE_2D;
        texture_info.format = render.swap_format;
        texture_info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
        texture_info.width = (Uint32)pixel_width;
        texture_info.height = (Uint32)pixel_height;
        texture_info.layer_count_or_depth = 1;
        texture_info.num_levels = 1;
        texture_info.sample_count = render.msaa;
        texture_info.props = 0;
        render.msaa_color = SDL_CreateGPUTexture(render.device_ptr, &texture_info);
        if (!render.msaa_color)
            SDIE("SDL_CreateGPUTexture(msaa_color)");
    }

    static void imgui_backend_shutdown()
    {
        ImGui_ImplSDLGPU3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
    }

    static void imgui_backend_init(const brender::renderer& renderer)
    {
        ImGui_ImplSDL3_InitForSDLGPU(renderer.window_ptr);
        ImGui_ImplSDLGPU3_InitInfo init_info;
        SDL_zero(init_info);
        init_info.Device = renderer.device_ptr;
        init_info.ColorTargetFormat = renderer.swap_format;
        init_info.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
        if (ImGui_ImplSDLGPU3_Init(&init_info) == false)
            SDIE("ImGui_ImplSDLGPU3_Init()");
    }

    void imgui_xinit(brender::renderer& renderer)
    {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        io.Fonts->AddFontDefault();
        ImGui::StyleColorsDark();
        imgui_backend_init(renderer);
        renderer.imgui_msaa = SDL_GPU_SAMPLECOUNT_1;
    }

    using draw_func_ptr = void(*)(const void*);

    static void create_scene_targets(brender::renderer& r, int w, int h) {
        if (r.scene_msaa) SDL_ReleaseGPUTexture(r.device_ptr, r.scene_msaa);
        if (r.scene_tex)  SDL_ReleaseGPUTexture(r.device_ptr, r.scene_tex);
        SDL_GPUTextureCreateInfo msaa{};
        msaa.type = SDL_GPU_TEXTURETYPE_2D;
        msaa.format = r.swap_format;
        msaa.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
        msaa.width = (Uint32)w;
        msaa.height = (Uint32)h;
        msaa.layer_count_or_depth = 1;
        msaa.num_levels = 1;
        msaa.sample_count = r.msaa;
        r.scene_msaa = SDL_CreateGPUTexture(r.device_ptr, &msaa);
        SDL_GPUTextureCreateInfo single{};
        single.type = SDL_GPU_TEXTURETYPE_2D;
        single.format = r.swap_format;
        single.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        single.width = (Uint32)w;
        single.height = (Uint32)h;
        single.layer_count_or_depth = 1;
        single.num_levels = 1;
        single.sample_count = SDL_GPU_SAMPLECOUNT_1;
        r.scene_tex = SDL_CreateGPUTexture(r.device_ptr, &single);
        if (!r.scene_sampler) {
            SDL_GPUSamplerCreateInfo sci{};
            r.scene_sampler = SDL_CreateGPUSampler(r.device_ptr, &sci);
        }
        r.scene_binding.texture = r.scene_tex;
        r.scene_binding.sampler = r.scene_sampler;
        r.scene_w = w;
        r.scene_h = h;
    }

    static void imgui_scene_window(brender::renderer& r) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::Begin("Scene");
        ImVec2 avail = ImGui::GetContentRegionAvail();
        int w = (int)avail.x, h = (int)avail.y;
        if (w < 1) w = 1;
        if (h < 1) h = 1;
        if (w != r.scene_w || h != r.scene_h) create_scene_targets(r, w, h);
        if (!r.scene_tex) create_scene_targets(r, w, h);
        ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
        if (pio.Renderer_RenderState && r.scene_sampler) {
            auto* rs = (ImGui_ImplSDLGPU3_RenderState*)pio.Renderer_RenderState;
            rs->SamplerCurrent = r.scene_sampler;
        }
        ImGui::Image((ImTextureID)r.scene_tex, avail);
        ImGui::End();
        ImGui::PopStyleVar();
    }

    void draw(brender::renderer& renderer, brender::draw_func_ptr draw_func, const void* draw_data)
    {
        if (g_mode == SceneMode::Docked) {
            ImGui_ImplSDL3_NewFrame();
            ImGui_ImplSDLGPU3_NewFrame();
            ImGui::NewFrame();
            ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(), ImGuiDockNodeFlags_PassthruCentralNode);
            imgui_scene_window(renderer);
            ImGui::Render();
        }

        brender::frame& frame = renderer.frame;
        frame.command_buffer_ptr = SDL_AcquireGPUCommandBuffer(renderer.device_ptr);

        SDL_GPUTexture* swap_texture = NULL;
        Uint32 swap_w = 0, swap_h = 0;
        bool ok = SDL_AcquireGPUSwapchainTexture(frame.command_buffer_ptr, renderer.window_ptr, &swap_texture, &swap_w, &swap_h);
        if (!ok || !swap_texture) {
            SDL_SubmitGPUCommandBuffer(frame.command_buffer_ptr);
            SDL_Delay(1);
            return;
        }

        if (g_mode == SceneMode::Docked) {
            SDL_GPUColorTargetInfo scene_t{};
            scene_t.texture = renderer.scene_msaa;
            scene_t.load_op = SDL_GPU_LOADOP_CLEAR;
            scene_t.store_op = SDL_GPU_STOREOP_RESOLVE;
            scene_t.clear_color = SDL_FColor{0.2f,0.3f,0.3f,1.0f};
            scene_t.resolve_texture = renderer.scene_tex;
            frame.render_pass_ptr = SDL_BeginGPURenderPass(frame.command_buffer_ptr, &scene_t, 1, NULL);
            if (draw_func && draw_data) draw_func(draw_data);
            SDL_EndGPURenderPass(frame.render_pass_ptr);

            SDL_GPUColorTargetInfo ui_t{};
            ui_t.texture = swap_texture;
            ui_t.load_op = SDL_GPU_LOADOP_CLEAR;
            ui_t.store_op = SDL_GPU_STOREOP_STORE;
            ui_t.clear_color = SDL_FColor{0.1f,0.1f,0.1f,1.0f};
            frame.render_pass_ptr = SDL_BeginGPURenderPass(frame.command_buffer_ptr, &ui_t, 1, NULL);
            ImGui_ImplSDLGPU3_PrepareDrawData(ImGui::GetDrawData(), frame.command_buffer_ptr);
            ImGui_ImplSDLGPU3_RenderDrawData(ImGui::GetDrawData(), frame.command_buffer_ptr, frame.render_pass_ptr, NULL);
            SDL_EndGPURenderPass(frame.render_pass_ptr);
        } else {
            if (renderer.msaa > SDL_GPU_SAMPLECOUNT_1) {
                SDL_GPUColorTargetInfo scene_t{};
                scene_t.texture = renderer.msaa_color;
                scene_t.load_op = SDL_GPU_LOADOP_CLEAR;
                scene_t.store_op = SDL_GPU_STOREOP_RESOLVE;
                scene_t.clear_color = SDL_FColor{0.2f,0.3f,0.3f,1.0f};
                scene_t.resolve_texture = swap_texture;
                frame.render_pass_ptr = SDL_BeginGPURenderPass(frame.command_buffer_ptr, &scene_t, 1, NULL);
                if (draw_func && draw_data) draw_func(draw_data);
                SDL_EndGPURenderPass(frame.render_pass_ptr);
            } else {
                SDL_GPUColorTargetInfo t{};
                t.texture = swap_texture;
                t.load_op = SDL_GPU_LOADOP_CLEAR;
                t.store_op = SDL_GPU_STOREOP_STORE;
                t.clear_color = SDL_FColor{0.2f,0.3f,0.3f,1.0f};
                frame.render_pass_ptr = SDL_BeginGPURenderPass(frame.command_buffer_ptr, &t, 1, NULL);
                if (draw_func && draw_data) draw_func(draw_data);
                SDL_EndGPURenderPass(frame.render_pass_ptr);
            }
        }

        SDL_SubmitGPUCommandBuffer(frame.command_buffer_ptr);
        SDL_Delay(1);
    }

    void xinit(brender::renderer& renderer, const brender::create_info& create_info)
    {
        if (SDL_Init(SDL_INIT_VIDEO) == false)
            SDIE("SDL_Init()");

        const brender::window& win = create_info.window;
        renderer.window_ptr = SDL_CreateWindow(win.title, win.width, win.height, win.flags);
        if (renderer.window_ptr == nullptr)
            SDIE("SDL_CreateWindow()");

        const brender::device& dev = create_info.device;
        renderer.device_ptr = SDL_CreateGPUDevice(dev.format_flags, dev.debug_mode, dev.name);
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
        create_target(renderer);

        imgui_xinit(renderer);
    }

}

namespace shader
{

    enum class type { vertex, fragment, pipeline };

    struct file {
        type shader_type{};
        std::string name;
        std::string path;
        std::string source;
        std::array<uint8_t, BLAKE3_OUT_LEN> dgst{};
    };

    struct spirv_info {
        std::vector<uint32_t> spirv;
        ReflectedResources reflect{};
    };

    struct source {
        shader::file file;
        spirv_info* data{};
        void* sdl_ptr{};
    };

    struct program {
        source vertex;
        source fragment;
        source pipeline;
    };

    struct manager {
        shaderc::Compiler compiler;
        shaderc::CompileOptions opts;
        std::vector<file> files;
        std::vector<program> programs;
    };

    inline SDL_GPUShader* as_shader(const source& source_ref)
    {
        return static_cast<SDL_GPUShader*>(source_ref.sdl_ptr);
    }

    inline SDL_GPUGraphicsPipeline* as_pipeline(const program& program_ref)
    {
        return static_cast<SDL_GPUGraphicsPipeline*>(program_ref.pipeline.sdl_ptr);
    }

    static void blake3_digest(const std::string& text, std::array<uint8_t, BLAKE3_OUT_LEN>& out_digest)
    {
        blake3_hasher hasher;
        blake3_hasher_init(&hasher);
        blake3_hasher_update(&hasher, text.data(), text.size());
        blake3_hasher_finalize(&hasher, out_digest.data(), out_digest.size());
    }

    static bool read_file_retry(const std::string& path, std::string& out, int tries = 10, int wait_ms = 8)
    {
        for (int i = 0; i < tries; ++i)
        {
            std::ifstream fs(path, std::ios::binary);
            if (fs)
            {
                std::string s((std::istreambuf_iterator<char>(fs)), std::istreambuf_iterator<char>());
                if (!s.empty())
                {
                    out.swap(s);
                    return true;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
        }
        return false;
    }

    static void load_text_file(file& out_file, type shader_type, const char* file_name)
    {
        out_file.shader_type = shader_type;
        out_file.name = file_name;
        out_file.path = std::string(SHADER_SRC_DIR) + "/" + file_name;
        std::string text;
        if (!read_file_retry(out_file.path, text))
            DIE(("File not found: " + out_file.path).c_str());
        out_file.source = std::move(text);
        if (out_file.source.empty())
            DIE(("Empty file: " + out_file.path).c_str());
        blake3_digest(out_file.source, out_file.dgst);
    }

    static shaderc_optimization_level map_opt_level(const std::string& text)
    {
        if (text == "zero" || text == "0")
            return shaderc_optimization_level_zero;
        if (text == "size")
            return shaderc_optimization_level_size;
        if (text == "performance" || text == "p")
            return shaderc_optimization_level_performance;
        const char* env_value = std::getenv("SHADERC_OPT");
        if (env_value)
        {
            std::string env_text(env_value);
            if (env_text == "0" || env_text == "zero")
                return shaderc_optimization_level_zero;
            if (env_text == "size")
                return shaderc_optimization_level_size;
            if (env_text == "performance" || env_text == "p")
                return shaderc_optimization_level_performance;
        }
        return shaderc_optimization_level_performance;
    }

    static std::unique_ptr<spirv_info> compile_to_spirv(manager& shader_manager, const file& shader_file, shaderc_shader_kind shader_kind)
    {
        auto compile_result = shader_manager.compiler.CompileGlslToSpv(shader_file.source, shader_kind, shader_file.name.c_str(), shader_manager.opts);
        if (compile_result.GetCompilationStatus() != shaderc_compilation_status_success)
            DIE(compile_result.GetErrorMessage().c_str());
        auto info_ptr = std::make_unique<spirv_info>();
        info_ptr->spirv.assign(compile_result.cbegin(), compile_result.cend());
        if (info_ptr->spirv.empty())
            DIE(("Compiled to empty SPIR-V: " + shader_file.name).c_str());
        if (!reflect_resources(info_ptr->spirv, info_ptr->reflect))
            DIE(("SPIR-V reflection failed: " + shader_file.name).c_str());
        return info_ptr;
    }

    void destroy_program(brender::renderer& renderer, program& program_ref)
    {
        if (program_ref.pipeline.sdl_ptr)
        {
            SDL_ReleaseGPUGraphicsPipeline(renderer.device_ptr, static_cast<SDL_GPUGraphicsPipeline*>(program_ref.pipeline.sdl_ptr));
            program_ref.pipeline.sdl_ptr = nullptr;
        }
        if (program_ref.vertex.sdl_ptr)
        {
            SDL_ReleaseGPUShader(renderer.device_ptr, static_cast<SDL_GPUShader*>(program_ref.vertex.sdl_ptr));
            program_ref.vertex.sdl_ptr = nullptr;
        }
        if (program_ref.fragment.sdl_ptr)
        {
            SDL_ReleaseGPUShader(renderer.device_ptr, static_cast<SDL_GPUShader*>(program_ref.fragment.sdl_ptr));
            program_ref.fragment.sdl_ptr = nullptr;
        }
        delete program_ref.vertex.data;
        program_ref.vertex.data = nullptr;
        delete program_ref.fragment.data;
        program_ref.fragment.data = nullptr;
    }

    program& build_program(brender::renderer& renderer, manager& shader_manager, const char* pipeline_json_name, program* reuse_program = nullptr)
    {
        program* program_ptr = reuse_program ? reuse_program : &shader_manager.programs.emplace_back();
        std::string pipeline_name_copy = pipeline_json_name;
        if (reuse_program)
        {
            destroy_program(renderer, *program_ptr);
            *program_ptr = program{};
        }

        program& out_program = *program_ptr;

        load_text_file(out_program.pipeline.file, type::pipeline, pipeline_name_copy.c_str());

        PipelineConfig cfg{};
        if (!load_pipeline_config(out_program.pipeline.file.path, cfg, 1))
            DIE(("Failed to load pipeline config: " + out_program.pipeline.file.path).c_str());

        load_text_file(out_program.vertex.file,   type::vertex,   cfg.vertex_shader.c_str());
        load_text_file(out_program.fragment.file, type::fragment, cfg.fragment_shader.c_str());

        std::string opt_global = cfg.shaderc_optimization;
        std::string opt_vs = cfg.shaderc_optimization_vs.size() ? cfg.shaderc_optimization_vs : opt_global;
        std::string opt_fs = cfg.shaderc_optimization_fs.size() ? cfg.shaderc_optimization_fs : opt_global;
        if (opt_vs.empty()) opt_vs = "performance";
        if (opt_fs.empty()) opt_fs = "performance";

        shader_manager.opts.SetOptimizationLevel(map_opt_level(opt_vs));
        auto vs_info = compile_to_spirv(shader_manager, out_program.vertex.file,   shaderc_vertex_shader);

        shader_manager.opts.SetOptimizationLevel(map_opt_level(opt_fs));
        auto fs_info = compile_to_spirv(shader_manager, out_program.fragment.file, shaderc_fragment_shader);

        out_program.vertex.data   = vs_info.release();
        out_program.fragment.data = fs_info.release();
        out_program.pipeline.data = nullptr;

        ReflectedVertexInput vertex_input{};
        if (!reflect_vertex_input(out_program.vertex.data->spirv, vertex_input))
            DIE("reflect_vertex_input");
        pack_tight(vertex_input);

        SDL_GPUSampleCount requested = map_samples(cfg.sample_count);
        SDL_GPUSampleCount chosen = choose_supported(renderer.device_ptr, renderer.swap_format, requested);
        shader::g_log.emplace_back("MSAA requested=" + std::to_string((int)requested) +
                           " chosen=" + std::to_string((int)chosen));

        if (renderer.msaa != chosen)
        {
            renderer.msaa = chosen;
            brender::create_target(renderer);
        }

        SDL_GPUShaderCreateInfo vci{};
        vci.code_size = out_program.vertex.data->spirv.size() * sizeof(uint32_t);
        vci.code = reinterpret_cast<const Uint8*>(out_program.vertex.data->spirv.data());
        vci.entrypoint = cfg.entry_vs.empty() ? "main" : cfg.entry_vs.c_str();
        vci.format = SDL_GPU_SHADERFORMAT_SPIRV;
        vci.stage  = SDL_GPU_SHADERSTAGE_VERTEX;
        vci.num_samplers         = out_program.vertex.data->reflect.num_samplers;
        vci.num_storage_textures = out_program.vertex.data->reflect.num_storage_textures;
        vci.num_storage_buffers  = out_program.vertex.data->reflect.num_storage_buffers;
        vci.num_uniform_buffers  = out_program.vertex.data->reflect.num_uniform_buffers;

        SDL_GPUShaderCreateInfo fci{};
        fci.code_size = out_program.fragment.data->spirv.size() * sizeof(uint32_t);
        fci.code = reinterpret_cast<const Uint8*>(out_program.fragment.data->spirv.data());
        fci.entrypoint = cfg.entry_fs.empty() ? "main" : cfg.entry_fs.c_str();
        fci.format = SDL_GPU_SHADERFORMAT_SPIRV;
        fci.stage  = SDL_GPU_SHADERSTAGE_FRAGMENT;
        fci.num_samplers         = out_program.fragment.data->reflect.num_samplers;
        fci.num_storage_textures = out_program.fragment.data->reflect.num_storage_textures;
        fci.num_storage_buffers  = out_program.fragment.data->reflect.num_storage_buffers;
        fci.num_uniform_buffers  = out_program.fragment.data->reflect.num_uniform_buffers;

        SDL_GPUShader* vertex_shader_ptr = SDL_CreateGPUShader(renderer.device_ptr, &vci);
        if (!vertex_shader_ptr)
            SDIE("SDL_CreateGPUShader(vertex)");

        SDL_GPUShader* fragment_shader_ptr = SDL_CreateGPUShader(renderer.device_ptr, &fci);
        if (!fragment_shader_ptr)
            SDIE("SDL_CreateGPUShader(fragment)");

        out_program.vertex.sdl_ptr   = vertex_shader_ptr;
        out_program.fragment.sdl_ptr = fragment_shader_ptr;

        SDL_GPUVertexInputState vertex_input_state{};
        vertex_input_state.vertex_buffer_descriptions = &vertex_input.buffer_desc;
        vertex_input_state.num_vertex_buffers = 1;
        vertex_input_state.vertex_attributes = vertex_input.attributes.data();
        vertex_input_state.num_vertex_attributes = (Uint32)vertex_input.attributes.size();

        SDL_GPURasterizerState rasterizer_state{};
        rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        rasterizer_state.cull_mode = cfg.cull;
        rasterizer_state.front_face= cfg.front_face;
        rasterizer_state.enable_depth_bias = false;
        rasterizer_state.enable_depth_clip = true;

        SDL_GPUMultisampleState multisample_state{};
        multisample_state.sample_count = renderer.msaa;
        multisample_state.sample_mask = 0;
        multisample_state.enable_mask = false;

        SDL_GPUDepthStencilState depth_stencil_state{};
        depth_stencil_state.enable_depth_test = cfg.depth.enable;
        depth_stencil_state.enable_depth_write= cfg.depth.write;
        depth_stencil_state.enable_stencil_test=false;
        depth_stencil_state.compare_op = cfg.depth.compare;
        depth_stencil_state.compare_mask = 0xFF;
        depth_stencil_state.write_mask = 0xFF;

        std::vector<SDL_GPUColorTargetDescription> color_targets;
        color_targets.resize(cfg.blends.empty() ? 1 : cfg.blends.size());
        for (size_t index = 0; index < color_targets.size(); ++index)
        {
            SDL_GPUColorTargetBlendState blend{};
            if (!cfg.blends.empty())
            {
                blend.enable_blend = cfg.blends[index].enable;
                blend.enable_color_write_mask = true;
                blend.color_write_mask = cfg.blends[index].write_mask;
                blend.src_color_blendfactor = cfg.blends[index].src_color;
                blend.dst_color_blendfactor = cfg.blends[index].dst_color;
                blend.color_blend_op = cfg.blends[index].color_op;
                blend.src_alpha_blendfactor = cfg.blends[index].src_alpha;
                blend.dst_alpha_blendfactor = cfg.blends[index].dst_alpha;
                blend.alpha_blend_op = cfg.blends[index].alpha_op;
            }
            else
            {
                blend.enable_blend = false;
                blend.enable_color_write_mask = true;
                blend.color_write_mask = SDL_GPU_COLORCOMPONENT_R | SDL_GPU_COLORCOMPONENT_G | SDL_GPU_COLORCOMPONENT_B | SDL_GPU_COLORCOMPONENT_A;
                blend.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
                blend.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
                blend.color_blend_op = SDL_GPU_BLENDOP_ADD;
                blend.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
                blend.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
                blend.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
            }
            color_targets[index].format = renderer.swap_format;
            color_targets[index].blend_state = blend;
        }

        SDL_GPUGraphicsPipelineTargetInfo target_info{};
        target_info.color_target_descriptions = color_targets.data();
        target_info.num_color_targets = (Uint32)color_targets.size();
        target_info.depth_stencil_format = cfg.depth.enable ? cfg.depth.format : SDL_GPU_TEXTUREFORMAT_INVALID;
        target_info.has_depth_stencil_target = cfg.depth.enable;

        SDL_GPUGraphicsPipelineCreateInfo pipeline_info{};
        pipeline_info.vertex_shader       = vertex_shader_ptr;
        pipeline_info.fragment_shader     = fragment_shader_ptr;
        pipeline_info.vertex_input_state  = vertex_input_state;
        pipeline_info.primitive_type      = cfg.primitive;
        pipeline_info.rasterizer_state    = rasterizer_state;
        pipeline_info.multisample_state   = multisample_state;
        pipeline_info.depth_stencil_state = depth_stencil_state;
        pipeline_info.target_info         = target_info;

        SDL_GPUGraphicsPipeline* pipeline_ptr = SDL_CreateGPUGraphicsPipeline(renderer.device_ptr, &pipeline_info);
        if (!pipeline_ptr)
            SDIE("SDL_CreateGPUGraphicsPipeline");

        out_program.pipeline.sdl_ptr = pipeline_ptr;
        return out_program;
    }

    static bool has_changed(const file& current)
    {
        std::string text1, text2;
        if (!read_file_retry(current.path, text1)) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
        if (!read_file_retry(current.path, text2)) return false;
        if (text1 != text2) return false;
        std::array<uint8_t, BLAKE3_OUT_LEN> dgst{};
        blake3_digest(text1, dgst);
        return dgst != current.dgst;
    }

    void init(manager& shader_manager)
    {
        shader_manager.opts.SetOptimizationLevel(shaderc_optimization_level_performance);
        shader_manager.opts.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_2);
    }

}

struct draw_function_data {
    brender::frame& frame;
    shader::program& program;
    SDL_GPUBuffer* vbo;
};

void draw_function(const void* data_ptr)
{
    const auto& args = *static_cast<const draw_function_data*>(data_ptr);
    SDL_BindGPUGraphicsPipeline(args.frame.render_pass_ptr, shader::as_pipeline(args.program));
    SDL_GPUBufferBinding vertex_binding;
    vertex_binding.buffer = args.vbo;
    vertex_binding.offset = 0;
    SDL_BindGPUVertexBuffers(args.frame.render_pass_ptr, 0, &vertex_binding, 1);
    SDL_DrawGPUPrimitives(args.frame.render_pass_ptr, 3, 1, 0, 0);
}

static SDL_HitTestResult SDLCALL window_hit_test(SDL_Window* win, const SDL_Point* pt, void* /*data*/)
{
    const int drag_bar_px = 30;
    const int resize_px = 8;

    int w = 0, h = 0;
    SDL_GetWindowSize(win, &w, &h);

    bool left = pt->x < resize_px;
    bool right = pt->x >= w - resize_px;
    bool top = pt->y < resize_px;
    bool bottom = pt->y >= h - resize_px;

    if (top && left) return SDL_HITTEST_RESIZE_TOPLEFT;
    if (top && right) return SDL_HITTEST_RESIZE_TOPRIGHT;
    if (bottom && left) return SDL_HITTEST_RESIZE_BOTTOMLEFT;
    if (bottom && right) return SDL_HITTEST_RESIZE_BOTTOMRIGHT;

    if (top) return SDL_HITTEST_RESIZE_TOP;
    if (bottom) return SDL_HITTEST_RESIZE_BOTTOM;
    if (left) return SDL_HITTEST_RESIZE_LEFT;
    if (right) return SDL_HITTEST_RESIZE_RIGHT;

    if (pt->y < drag_bar_px) return SDL_HITTEST_DRAGGABLE;

    return SDL_HITTEST_NORMAL;
}

int main(int argc, char* argv[])
{
    brender::renderer renderer;
    brender::create_info create_info;
    brender::xinit(renderer, create_info);

    const float aspect = 16.0f / 9.0f;
    SDL_SetWindowAspectRatio(renderer.window_ptr, aspect, aspect);
    SDL_SetWindowMinimumSize(renderer.window_ptr, 640, 360);

    float vertices[] =
    {
        -0.5f, -0.5f, 1.0f, 0.2f, 0.2f,
         0.5f, -0.5f, 0.2f, 1.0f, 0.2f,
         0.0f,  0.5f, 0.2f, 0.2f, 1.0f
    };

    SDL_GPUBufferCreateInfo vertex_buffer_info;
    SDL_zero(vertex_buffer_info);
    vertex_buffer_info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    vertex_buffer_info.size = sizeof(vertices);
    SDL_GPUBuffer* vbo = SDL_CreateGPUBuffer(renderer.device_ptr, &vertex_buffer_info);
    if (!vbo)
        return 1;

    SDL_GPUTransferBufferCreateInfo transfer_info;
    SDL_zero(transfer_info);
    transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transfer_info.size = sizeof(vertices);
    SDL_GPUTransferBuffer* tbo = SDL_CreateGPUTransferBuffer(renderer.device_ptr, &transfer_info);
    if (!tbo)
        return 1;

    void* mapped_ptr = SDL_MapGPUTransferBuffer(renderer.device_ptr, tbo, false);
    if (!mapped_ptr)
        return 1;
    SDL_memcpy(mapped_ptr, vertices, sizeof(vertices));
    SDL_UnmapGPUTransferBuffer(renderer.device_ptr, tbo);

    SDL_GPUCommandBuffer* copy_commands = SDL_AcquireGPUCommandBuffer(renderer.device_ptr);
    SDL_GPUCopyPass* copy_pass = SDL_BeginGPUCopyPass(copy_commands);
    SDL_GPUTransferBufferLocation source_location;
    source_location.transfer_buffer = tbo;
    source_location.offset = 0;
    SDL_GPUBufferRegion destination_region;
    destination_region.buffer = vbo;
    destination_region.offset = 0;
    destination_region.size = sizeof(vertices);
    SDL_UploadToGPUBuffer(copy_pass, &source_location, &destination_region, true);
    SDL_EndGPUCopyPass(copy_pass);
    SDL_SubmitGPUCommandBuffer(copy_commands);

    SDL_GPUBufferBinding vertex_binding;
    vertex_binding.buffer = vbo;
    vertex_binding.offset = 0;

    shader::manager shader_manager;
    shader::init(shader_manager);
    shader::program& triangle_program = shader::build_program(renderer, shader_manager, "triangle.pipeline.json");

    draw_function_data draw_data{ renderer.frame, triangle_program, vbo };

    int running = 1;
    while (running)
    {
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT) running = 0;
            if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) running = 0;
            if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
            {
                brender::create_target(renderer);
            }
            if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F1)
            {
                g_mode = (g_mode == SceneMode::Docked) ? SceneMode::Fullscreen : SceneMode::Docked;
                if (g_mode == SceneMode::Fullscreen)
                {
                    SDL_SetWindowBordered(renderer.window_ptr, false);
                    SDL_SetWindowHitTest(renderer.window_ptr, window_hit_test, nullptr);
                }
                else
                {
                    SDL_SetWindowBordered(renderer.window_ptr, true);
                    SDL_SetWindowHitTest(renderer.window_ptr, nullptr, nullptr);
                }
            }
        }

        for (shader::program& prog : shader_manager.programs)
        {
            bool pipeline_changed = shader::has_changed(prog.pipeline.file);
            bool vertex_changed   = shader::has_changed(prog.vertex.file);
            bool fragment_changed = shader::has_changed(prog.fragment.file);
            if (pipeline_changed || vertex_changed || fragment_changed)
                shader::build_program(renderer, shader_manager, prog.pipeline.file.name.c_str(), &prog);
        }
        brender::draw(renderer, &draw_function, &draw_data);
    }

    brender::imgui_backend_shutdown();
    ImGui::DestroyContext();

    shader::destroy_program(renderer, triangle_program);
    SDL_ReleaseGPUTransferBuffer(renderer.device_ptr, tbo);
    SDL_ReleaseGPUBuffer(renderer.device_ptr, vbo);
    if (renderer.msaa_color) SDL_ReleaseGPUTexture(renderer.device_ptr, renderer.msaa_color);
    if (renderer.scene_msaa) SDL_ReleaseGPUTexture(renderer.device_ptr, renderer.scene_msaa);
    if (renderer.scene_tex) SDL_ReleaseGPUTexture(renderer.device_ptr, renderer.scene_tex);
    if (renderer.scene_sampler) SDL_ReleaseGPUSampler(renderer.device_ptr, renderer.scene_sampler);

    SDL_ReleaseWindowFromGPUDevice(renderer.device_ptr, renderer.window_ptr);
    SDL_DestroyWindow(renderer.window_ptr);
    SDL_DestroyGPUDevice(renderer.device_ptr);
    SDL_Quit();
    return 0;
}

