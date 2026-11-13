#version 450
// A triangle of the scene (view.h, view_gpu_vertex): the target's pixels and
// z = 1/w. The position's w is 1/z, so the texture coordinates are
// interpolated with the perspective, as the software rasterizer does; the
// colours and z are interpolated on the screen (noperspective), as it does.
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_col;
layout(location = 3) in vec4 a_ofs;
layout(location = 4) in uint a_state;

layout(set = 1, binding = 0) uniform VU {
    vec4 screen;        // target width, height (pixels), depth scale (1 / largest z), 0
} vu;

layout(location = 0) noperspective out vec4 v_col;
layout(location = 1) noperspective out vec4 v_ofs;
layout(location = 2) out vec2 v_uv;
layout(location = 3) noperspective out float v_z;
layout(location = 4) flat out uint v_state;

void main()
{
    float w = a_pos.z > 1e-30 ? 1.0 / a_pos.z : 1.0;
    vec2 ndc = vec2(a_pos.x / vu.screen.x * 2.0 - 1.0, 1.0 - a_pos.y / vu.screen.y * 2.0);
    gl_Position = vec4(ndc * w, clamp(a_pos.z * vu.screen.z, 0.0, 1.0) * w, w);
    v_col = a_col * 255.0;
    v_ofs = a_ofs * 255.0;
    v_uv = a_uv;
    v_z = a_pos.z;
    v_state = a_state;
}
