#include "pipeline_config.h"
#include <nlohmann/json.hpp>
#include <fstream>
using nlohmann::json;

static Uint32 parse_mask(const std::string& s) {
    Uint32 m = 0;
    for (char c : s) {
        if (c == 'r' || c == 'R') m |= SDL_GPU_COLORCOMPONENT_R;
        if (c == 'g' || c == 'G') m |= SDL_GPU_COLORCOMPONENT_G;
        if (c == 'b' || c == 'B') m |= SDL_GPU_COLORCOMPONENT_B;
        if (c == 'a' || c == 'A') m |= SDL_GPU_COLORCOMPONENT_A;
    }
    return m ? m : SDL_GPU_COLORCOMPONENT_R | SDL_GPU_COLORCOMPONENT_G | SDL_GPU_COLORCOMPONENT_B | SDL_GPU_COLORCOMPONENT_A;
}

static SDL_GPUPrimitiveType parse_prim(const std::string& s) {
    if (s == "line_list") return SDL_GPU_PRIMITIVETYPE_LINELIST;
    if (s == "line_strip") return SDL_GPU_PRIMITIVETYPE_LINESTRIP;
    if (s == "triangle_strip") return SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP;
    return SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
}

static SDL_GPUCullMode parse_cull(const std::string& s) {
    if (s == "front") return SDL_GPU_CULLMODE_FRONT;
    if (s == "back") return SDL_GPU_CULLMODE_BACK;
    return SDL_GPU_CULLMODE_NONE;
}

static SDL_GPUFrontFace parse_face(const std::string& s) {
    if (s == "cw") return SDL_GPU_FRONTFACE_CLOCKWISE;
    return SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
}

static SDL_GPUCompareOp parse_cmp(const std::string& s) {
    if (s == "never") return SDL_GPU_COMPAREOP_NEVER;
    if (s == "less") return SDL_GPU_COMPAREOP_LESS;
    if (s == "equal") return SDL_GPU_COMPAREOP_EQUAL;
    if (s == "less_equal") return SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
    if (s == "greater") return SDL_GPU_COMPAREOP_GREATER;
    if (s == "not_equal") return SDL_GPU_COMPAREOP_NOT_EQUAL;
    if (s == "greater_equal") return SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;
    return SDL_GPU_COMPAREOP_ALWAYS;
}

static SDL_GPUBlendFactor parse_bf(const std::string& s) {
    if (s == "zero") return SDL_GPU_BLENDFACTOR_ZERO;
    if (s == "one") return SDL_GPU_BLENDFACTOR_ONE;
    if (s == "src_color") return SDL_GPU_BLENDFACTOR_SRC_COLOR;
    if (s == "one_minus_src_color") return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR;
    if (s == "dst_color") return SDL_GPU_BLENDFACTOR_DST_COLOR;
    if (s == "one_minus_dst_color") return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_COLOR;
    if (s == "src_alpha") return SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    if (s == "one_minus_src_alpha") return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    if (s == "dst_alpha") return SDL_GPU_BLENDFACTOR_DST_ALPHA;
    if (s == "one_minus_dst_alpha") return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA;
    return SDL_GPU_BLENDFACTOR_ONE;
}

static SDL_GPUBlendOp parse_bo(const std::string& s) {
    if (s == "add") return SDL_GPU_BLENDOP_ADD;
    if (s == "subtract") return SDL_GPU_BLENDOP_SUBTRACT;
    if (s == "reverse_subtract") return SDL_GPU_BLENDOP_REVERSE_SUBTRACT;
    if (s == "min") return SDL_GPU_BLENDOP_MIN;
    if (s == "max") return SDL_GPU_BLENDOP_MAX;
    return SDL_GPU_BLENDOP_ADD;
}

SDL_GPUSampleCount map_samples(Uint32 n) {
    switch (n) {
        case 1: return SDL_GPU_SAMPLECOUNT_1;
        case 2: return SDL_GPU_SAMPLECOUNT_2;
        case 4: return SDL_GPU_SAMPLECOUNT_4;
        case 8: return SDL_GPU_SAMPLECOUNT_8;
        default: return SDL_GPU_SAMPLECOUNT_1;
    }
}


SDL_GPUSampleCount choose_supported(SDL_GPUDevice* dev, SDL_GPUTextureFormat fmt, SDL_GPUSampleCount desired) {
    if (SDL_GPUTextureSupportsSampleCount(dev, fmt, desired)) return desired;
    SDL_GPUSampleCount cands[] = { SDL_GPU_SAMPLECOUNT_8, SDL_GPU_SAMPLECOUNT_4, SDL_GPU_SAMPLECOUNT_2, SDL_GPU_SAMPLECOUNT_1 };
    for (size_t i = 0; i < 4; i++) {
        if (cands[i] <= desired && SDL_GPUTextureSupportsSampleCount(dev, fmt, cands[i])) return cands[i];
    }
    return SDL_GPU_SAMPLECOUNT_1;
}

bool load_pipeline_config(const std::string& path, PipelineConfig& out, Uint32 reflected_color_attachments) {
    std::ifstream f(path);
    if (!f.is_open()) {
        std::fprintf(stderr, "Pipeline config not found: %s\n", path.c_str());
        return false;
    }
    if (!f.good()) {
        out.blends.assign(reflected_color_attachments ? reflected_color_attachments : 1, PipelineConfig::Blend{});
        return false;
    }
    try {
        json j;
        f >> j;
        out.vertex_shader = j.contains("vertex_shader") ? j["vertex_shader"].get<std::string>() : "";
        out.fragment_shader = j.contains("fragment_shader") ? j["fragment_shader"].get<std::string>() : "";
        if (j.contains("entry_points")) {
            const auto& ep = j["entry_points"];
            out.entry_vs = ep.contains("vs") ? ep["vs"].get<std::string>() : "main";
            out.entry_fs = ep.contains("fs") ? ep["fs"].get<std::string>() : "main";
        } else {
            out.entry_vs = "main";
            out.entry_fs = "main";
        }
            if (j.contains("primitive")) out.primitive = parse_prim(j["primitive"].get<std::string>());
        if (j.contains("cull")) out.cull = parse_cull(j["cull"].get<std::string>());
        if (j.contains("front_face")) out.front_face = parse_face(j["front_face"].get<std::string>());
        if (j.contains("msaa") && j["msaa"].contains("sample_count")) out.sample_count = std::max<Uint32>(1, j["msaa"]["sample_count"].get<Uint32>());
        if (j.contains("depth")) {
            auto d = j["depth"];
            if (d.contains("enable")) out.depth.enable = d["enable"].get<bool>();
            if (d.contains("write")) out.depth.write = d["write"].get<bool>();
            if (d.contains("compare")) out.depth.compare = parse_cmp(d["compare"].get<std::string>());
            if (d.contains("format")) out.depth.format = SDL_GPU_TEXTUREFORMAT_INVALID;
        }
        Uint32 n = reflected_color_attachments ? reflected_color_attachments : 1;
        out.blends.assign(n, PipelineConfig::Blend{});
        if (j.contains("blend") && j["blend"].is_array()) {
            for (size_t i = 0; i < j["blend"].size() && i < out.blends.size(); i++) {
                auto b = j["blend"][i];
                if (b.contains("enable")) out.blends[i].enable = b["enable"].get<bool>();
                if (b.contains("write_mask")) out.blends[i].write_mask = parse_mask(b["write_mask"].get<std::string>());
                if (b.contains("src_color")) out.blends[i].src_color = parse_bf(b["src_color"].get<std::string>());
                if (b.contains("dst_color")) out.blends[i].dst_color = parse_bf(b["dst_color"].get<std::string>());
                if (b.contains("color_op")) out.blends[i].color_op = parse_bo(b["color_op"].get<std::string>());
                else if (b.contains("color_blend_op")) out.blends[i].color_op = parse_bo(b["color_blend_op"].get<std::string>());
                if (b.contains("src_alpha")) out.blends[i].src_alpha = parse_bf(b["src_alpha"].get<std::string>());
                if (b.contains("dst_alpha")) out.blends[i].dst_alpha = parse_bf(b["dst_alpha"].get<std::string>());
                if (b.contains("alpha_op")) out.blends[i].alpha_op = parse_bo(b["alpha_op"].get<std::string>());
                else if (b.contains("alpha_blend_op")) out.blends[i].alpha_op = parse_bo(b["alpha_blend_op"].get<std::string>());
            }
        }
        if (j.contains("vertex_layout")) {
            const std::string v = j["vertex_layout"].get<std::string>();
            out.vertex_layout_auto = (v != "manual");
        }
        if (j.contains("shaderc")) {
            auto s = j["shaderc"];
            if (s.contains("optimization")) out.shaderc_optimization = s["optimization"].get<std::string>();
            if (s.contains("vertex")) out.shaderc_optimization_vs = s["vertex"].get<std::string>();
            if (s.contains("fragment")) out.shaderc_optimization_fs = s["fragment"].get<std::string>();
        }
        return true;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Pipeline config parse error: %s\n", e.what());
        return false;
    }
}

