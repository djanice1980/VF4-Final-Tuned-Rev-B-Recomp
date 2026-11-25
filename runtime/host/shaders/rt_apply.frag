#version 450
// The traced light into the picture, where the stencil says the 3D scene is
// (docs/design.md section 74): the picture's color times the light's ratio
// (rt_light.comp's first output, a quarter of the ratio in 10 bits a
// channel), less the glass's share, plus what the glass mirrors (its second
// output: the mirrored color times the share, and the share).
layout(location = 0) out vec4 o_col;
layout(set = 2, binding = 0) uniform sampler2D t_color;
layout(set = 2, binding = 1) uniform sampler2D t_light;
layout(set = 2, binding = 2) uniform sampler2D t_refl;

void main()
{
    ivec2 px = ivec2(gl_FragCoord.xy);
    vec3 c = texelFetch(t_color, px, 0).rgb;
    vec3 ratio = texelFetch(t_light, px, 0).rgb * 4.0;
    vec4 r = texelFetch(t_refl, px, 0);
    o_col = vec4(c * ratio * (1.0 - r.a) + r.rgb, 1.0);
}
