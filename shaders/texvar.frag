#version 450
layout(set = 0, binding = 0) uniform samplerCube u_cube;
layout(set = 0, binding = 1) uniform sampler2DArray u_arr;
layout(set = 0, binding = 2) uniform sampler2DShadow u_shad;
layout(location = 0) in vec3 v_dir;
layout(location = 1) in vec3 v_uvw;
layout(location = 2) in vec3 v_shad;
layout(location = 0) out vec4 o_color;
void main() {
    vec4 c = texture(u_cube, v_dir);
    c += texture(u_arr, v_uvw);
    c += vec4(texture(u_shad, v_shad));
    o_color = c;
}
