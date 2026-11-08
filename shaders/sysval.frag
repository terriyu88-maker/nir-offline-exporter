#version 450
layout(location = 0) out vec4 o_color;
void main() {
    o_color = gl_FragCoord;
    if (!gl_FrontFacing)
        o_color.a = 0.0;
    gl_FragDepth = gl_FragCoord.z;
}
