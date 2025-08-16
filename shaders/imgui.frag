#version 450
layout(location=0) in vec2 v_uv;
layout(location=1) in vec4 v_color;
layout(location=0) out vec4 out_color;
layout(set=2, binding=0) uniform sampler2D uTex0;
void main() {
    vec4 t = texture(uTex0, v_uv);
    out_color = v_color * t;
}

