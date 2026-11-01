#version 450
layout(location = 0) in vec4 v_color;
layout(location = 1) in float v_count;
layout(location = 0) out vec4 o_color;
void main() {
    vec4 c = v_color;
    if (v_color.a > 0.5)
        c *= 2.0;
    else
        c *= 0.5;
    int n = int(v_count);
    for (int i = 0; i < n; i++)
        c *= 0.9;
    o_color = c;
}
