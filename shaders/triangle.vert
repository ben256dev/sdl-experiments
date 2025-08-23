#version 450
layout(location = 0) in vec3 in_col;
layout(location = 1) in vec2 in_pos;
layout(location = 2) in vec2 in_uv;
layout(location = 0) out vec3 v_col;
void main() {
    v_col = in_col;
    gl_Position = vec4(in_pos, 0.0, 1.0);
}

