#version 450
layout(location = 0) flat in vec4 v_flat;
layout(location = 1) noperspective in vec4 v_nop;
layout(location = 0) out vec4 o_color;
void main() {
    o_color = v_flat + v_nop;
}
