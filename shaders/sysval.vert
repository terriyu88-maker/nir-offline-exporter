#version 450
layout(location = 0) out vec4 v_out;
void main() {
    v_out = vec4(float(gl_VertexIndex), float(gl_InstanceIndex), 0.0, 1.0);
    gl_Position = v_out;
}
