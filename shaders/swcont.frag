#version 450
layout(location = 0) in float v_sel;
layout(location = 1) in float v_count;
layout(location = 0) out vec4 o_color;
void main() {
    vec4 c = vec4(0.0);
    int s = int(v_sel);
    switch (s) {
    case 0:
        c = vec4(1.0, 0.0, 0.0, 1.0);
        break;
    case 1:
        c = vec4(0.0, 1.0, 0.0, 1.0);
        break;
    default:
        c = vec4(0.0, 0.0, 1.0, 1.0);
        break;
    }
    int n = int(v_count);
    for (int i = 0; i < n; i++) {
        if (i == 1)
            continue;
        c *= 0.9;
    }
    o_color = c;
}
