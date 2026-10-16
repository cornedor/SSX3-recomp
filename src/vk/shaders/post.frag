#version 450

/* Remaster post pass on the finished frame (vkr.c post_pass), each part with
 * its own knob and 0 = off:
 *   sharpen   contrast-adaptive sharpening (AMD CAS idea): crisper textures
 *             at high internal resolutions, without halos on hard edges
 *   bloom     light spill from the brightest parts (sun on snow, neon, the
 *             lights of the night events); from the frame's mips, above
 *             thresh, so ordinary white snow stays put
 *   vibrance  saturation for the dull colours only (skin and the vivid
 *             signage are left alone)
 *   contrast  a gentle S-curve around mid grey
 *   deband    +-1/2 LSB of triangular noise: no steps in sky gradients
 *   vignette  darker corners
 * The game's colours stay the reference: every part is small and keeps hue. */

layout(location = 0) out vec4 o;
layout(set = 0, binding = 0) uniform sampler2D img;     /* the frame, with mips */

layout(push_constant) uniform PC {
    vec4 rect;                  /* the picture: x, y, w, h (pillarbox: the 4:3 middle) */
    vec2 tsize;                 /* target size */
    float sharpen, bloom, thresh, vibrance, contrast, deband, vignette, time;
    float levels;               /* mips of img */
} pc;

float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

float hash(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

void main()
{
    ivec2 ip = ivec2(gl_FragCoord.xy);
    vec2 uv = gl_FragCoord.xy / pc.tsize;
    vec3 c = texelFetch(img, ip, 0).rgb;
    float alpha = texelFetch(img, ip, 0).a;

    if (gl_FragCoord.x < pc.rect.x || gl_FragCoord.x >= pc.rect.x + pc.rect.z) {
        o = vec4(c, alpha);                 /* pillarbox bars: untouched */
        return;
    }

    if (pc.sharpen > 0.0) {
        ivec2 mx = ivec2(pc.tsize) - 1;
        vec3 n = texelFetch(img, clamp(ip + ivec2(0, -1), ivec2(0), mx), 0).rgb;
        vec3 s = texelFetch(img, clamp(ip + ivec2(0, 1), ivec2(0), mx), 0).rgb;
        vec3 w = texelFetch(img, clamp(ip + ivec2(-1, 0), ivec2(0), mx), 0).rgb;
        vec3 e = texelFetch(img, clamp(ip + ivec2(1, 0), ivec2(0), mx), 0).rgb;
        vec3 lo = min(c, min(min(n, s), min(w, e)));
        vec3 hi = max(c, max(max(n, s), max(w, e)));
        /* less where the neighbourhood already spans the range (edges) */
        vec3 amp = sqrt(clamp(min(lo, 1.0 - hi) / max(hi, 1e-4), 0.0, 1.0));
        vec3 wt = -amp / mix(8.0, 5.0, clamp(pc.sharpen, 0.0, 1.0));
        c = clamp((c + (n + s + w + e) * wt) / (1.0 + 4.0 * wt), 0.0, 1.0);
    }

    if (pc.bloom > 0.0) {
        /* levels 2..6 of the frame: wide and soft, the narrow ones weigh most */
        vec3 b = vec3(0.0);
        float tw = 0.0, wt = 1.0;
        for (float l = 2.0; l <= 6.0 && l < pc.levels; l += 1.0) {
            vec3 m = textureLod(img, uv, l).rgb;
            /* soft knee: what is above thresh, faded in over 0.1 */
            float br = max(max(m.r, m.g), m.b);
            float k = clamp((br - pc.thresh + 0.1) / 0.2, 0.0, 1.0);
            float over = max(br - pc.thresh, 0.0) + k * k * 0.05;
            b += m * (over / max(br, 1e-4)) * wt;
            tw += wt;
            wt *= 0.8;
        }
        if (tw > 0.0)
            c += pc.bloom * b / tw * (1.0 - c * 0.5);     /* soft add: no hard clip to white */
    }

    if (pc.contrast != 0.0) {
        vec3 sc = c * c * (3.0 - 2.0 * c);
        c = mix(c, sc, pc.contrast);
    }

    if (pc.vibrance != 0.0) {
        float l = luma(c);
        float sat = max(max(c.r, c.g), c.b) - min(min(c.r, c.g), c.b);
        c = mix(vec3(l), c, 1.0 + pc.vibrance * (1.0 - sat));
    }

    if (pc.vignette > 0.0) {
        vec2 q = (gl_FragCoord.xy - pc.rect.xy) / pc.rect.zw - 0.5;
        q.x *= pc.rect.z / pc.rect.w;
        c *= 1.0 - pc.vignette * smoothstep(0.35, 1.0, length(q));
    }

    c = clamp(c, 0.0, 1.0);
    if (pc.deband > 0.0) {
        float t = fract(pc.time * 0.618);
        float nz = hash(gl_FragCoord.xy + t * 97.0) + hash(gl_FragCoord.yx * 1.37 + t * 41.0) - 1.0;
        c += nz * (pc.deband / 255.0);
    }
    o = vec4(c, alpha);
}
