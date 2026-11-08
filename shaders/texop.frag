#version 450
layout(set = 0, binding = 0) uniform sampler2D u_tex;
layout(location = 0) in vec2 v_uv;
layout(location = 1) in float v_lod;
layout(location = 2) in vec2 v_ddx;
layout(location = 3) in vec2 v_ddy;
layout(location = 0) out vec4 o_color;
void main() {
    vec4 c = textureLod(u_tex, v_uv, v_lod);
    c += textureGrad(u_tex, v_uv, v_ddx, v_ddy);
    c += textureOffset(u_tex, v_uv, ivec2(1, 0));
    c += texelFetch(u_tex, ivec2(v_uv), 0);
    c += textureGather(u_tex, v_uv, 0);
    o_color = c;
}
