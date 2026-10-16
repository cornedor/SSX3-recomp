#version 450

/* RECOMP_SHADOW_SOFT: a soft copy of a GPU render target (vkr.c rt_soften),
 * for SSX 3's rider shadow maps. The receiver keeps the map's alpha test
 * (GREATER 20) and blends ZERO / ONE_MINUS_SRC_COLOR: the shadow is the
 * map's colour, and the transparent black around the rider means "none". So
 * the colour is simply averaged (it fades to black = no shadow at the edge)
 * and the alpha is the neighbourhood's maximum, so the alpha test does not
 * cut the faint outer part of the soft edge. Gaussian, 7x7 taps. */

layout(location = 0) out vec4 o;
layout(set = 0, binding = 0) uniform sampler2D img;   /* the sharp map, nearest */

layout(push_constant) uniform PC {
    vec2 inv_dst;       /* 1 / this image's size */
    vec2 inv_src;       /* 1 / the map's size */
    float radius;       /* in map texels */
    float thr;          /* unused */
} pc;

void main()
{
    vec2 uv = gl_FragCoord.xy * pc.inv_dst;
    vec3 acc = vec3(0.0);
    float wsum = 0.0, a = 0.0;
    float step = pc.radius / 3.0;
    for (int j = -3; j <= 3; j++)
        for (int i = -3; i <= 3; i++) {
            float w = exp(-float(i * i + j * j) / 4.5);     /* sigma 1.5 taps */
            vec4 t = texture(img, uv + vec2(i, j) * step * pc.inv_src);
            acc += t.rgb * w;
            wsum += w;
            a = max(a, t.a);
        }
    o = vec4(acc / wsum, a);
}
