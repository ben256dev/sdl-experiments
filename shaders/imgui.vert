#version 450
layout(location=0) in vec2 in_pos;
layout(location=1) in vec2 in_uv;
layout(location=2) in vec4 in_color;
layout(location=0) out vec2 v_uv;
layout(location=1) out vec4 v_color;
layout(set=1, binding=0, std140) uniform VSBlock { mat4 uProj; };
void main() {
    v_uv = in_uv;
    v_color = in_color;
    gl_Position = uProj * vec4(in_pos, 0.0, 1.0);
}

