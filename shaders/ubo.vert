#version 450
layout(set = 0, binding = 0) uniform UBO {
    mat4 mvp;
} u;
layout(location = 0) in vec4 a_pos;
layout(location = 0) out vec4 v_pos;
void main() {
    v_pos = u.mvp * a_pos;
    gl_Position = v_pos;
}
