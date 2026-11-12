#version 450
layout(triangles, equal_spacing, ccw) in;
layout(location = 0) in vec4 v_in[];
layout(location = 0) out vec4 v_out;
void main() {
    gl_Position = gl_TessCoord.x * gl_in[0].gl_Position
                + gl_TessCoord.y * gl_in[1].gl_Position
                + gl_TessCoord.z * gl_in[2].gl_Position;
    v_out = gl_TessCoord.x * v_in[0]
          + gl_TessCoord.y * v_in[1]
          + gl_TessCoord.z * v_in[2];
}
