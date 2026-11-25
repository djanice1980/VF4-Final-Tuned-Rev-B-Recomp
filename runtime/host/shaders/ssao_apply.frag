#version 450
// The smoothed occlusion as a colour: the blend multiplies it into the
// picture, where the stencil says the 3D scene is (docs/design.md section 73).
layout(location = 0) out vec4 o_col;
layout(set = 2, binding = 0) uniform sampler2D t_ao;

void main()
{
    float ao = texelFetch(t_ao, ivec2(gl_FragCoord.xy), 0).r;
    o_col = vec4(ao, ao, ao, 1.0);
}
