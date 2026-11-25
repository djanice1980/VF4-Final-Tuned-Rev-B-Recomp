#version 450
// The occlusion smoothed over the 4 x 4 block its pattern repeats in, among
// the pixels at about the same depth only: an edge keeps its side
// (docs/design.md section 73).
layout(location = 0) out vec4 o_col;
layout(set = 2, binding = 0) uniform sampler2D t_ao;
layout(set = 2, binding = 1) uniform sampler2D t_depth;

void main()
{
    ivec2 size = textureSize(t_ao, 0);
    ivec2 px = ivec2(gl_FragCoord.xy);
    float zc = texelFetch(t_depth, px, 0).r;
    float sum = 0.0, n = 0.0;
    for (int y = -2; y < 2; y++)
        for (int x = -2; x < 2; x++) {
            ivec2 q = clamp(px + ivec2(x, y), ivec2(0), size - 1);
            float z = texelFetch(t_depth, q, 0).r;
            // depth is 1 / distance: within a tenth of the distance
            if (abs(z - zc) <= 0.1 * zc) {
                sum += texelFetch(t_ao, q, 0).r;
                n += 1.0;
            }
        }
    float ao = n > 0.0 ? sum / n : texelFetch(t_ao, px, 0).r;
    o_col = vec4(ao, ao, ao, 1.0);
}
