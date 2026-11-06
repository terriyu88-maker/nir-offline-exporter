#version 450
#extension GL_EXT_demote_to_helper_invocation : require
layout(location = 0) in vec4 v_color;
layout(location = 0) out vec4 o_color;
void main() {
    if (v_color.a < 0.0)
        discard;
    if (v_color.a < 0.5)
        demote;
    o_color = v_color;
}
