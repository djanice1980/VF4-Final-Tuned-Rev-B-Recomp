#version 450
// The background plane (view.c, target_clear): a colour over the screen
// from three corners, with the table's fog of its depth
layout(location = 0) out vec4 o_col;
layout(set = 3, binding = 0) uniform FU {
    vec4 c0;            // colour at (x0, y0), 0..255; w: render scale
    vec4 ga;            // its change along x; w: x0
    vec4 gb;            // along y; w: y0
    vec4 fog;           // fog colour 0..255, fog amount 0..256
} fu;

void main()
{
    vec2 p = gl_FragCoord.xy / fu.c0.w;
    vec3 v = clamp(floor(fu.c0.rgb + fu.ga.rgb * (p.x - fu.ga.w) + fu.gb.rgb * (p.y - fu.gb.w)), 0.0, 255.0);
    v = v + floor((fu.fog.rgb - v) * fu.fog.w / 256.0);
    o_col = vec4(v / 255.0, 1.0);
}
