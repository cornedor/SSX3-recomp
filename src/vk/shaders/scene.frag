#version 450
#extension GL_GOOGLE_include_directive : require

/* The scene pass (vkr.c scene_pass), on the finished 3D image before the HUD
 * is drawn over it (vkr_scene_run): motion blur that sells speed.
 *
 * Each pixel's blur is the sum of
 *   camera blur (RECOMP_MBLUR)  the pixel reprojected with last frame's
 *                               camera: turns, and the near snow rushing by;
 *   speed blur (speed=)         radial streaks away from where the camera
 *                               is heading, growing with the camera's speed
 *                               (vkr_backend.c scene_flip) -- far terrain
 *                               and sky barely move between frames, so
 *                               reprojection alone shows little speed;
 *                               full near the camera, less far away (reach=);
 * stronger towards the screen edges (edge=) and at most cap= pixels long.
 *
 * Gather along the blur in the manner of McGuire et al., "A Reconstruction
 * Filter for Plausible Motion Blur" (I3D 2012): each sample is weighed by
 * which of the two is in front (a soft depth compare) and whether the
 * nearer one's blur reaches the other, so near geometry does not smear
 * into what lies behind it and vice versa. Player 1 (tag) and HUD models
 * stay sharp and are never sampled; the other riders blur by their own
 * motion on screen (vkr_backend.c scene_flip), so one riding alongside
 * stays sharp and one passing streaks. */

#include "scene_common.glsl"

layout(location = 0) out vec4 o;

const int TAPS = 16;

float scl;                      /* pixel sizes are for 1080 lines */

/* the blur at target pixel p with depth d, in target pixels; zero where
 * nothing blurs (player 1, HUD models, nearer than the near distance) */
vec2 blur_at(vec2 p, float d, float tag, out float dist)
{
    bool sky = is_sky(d);
    vec3 P = pos_at(p, sky ? 1.0 : d);
    vec2 v;
    dist = sky ? 1e9 : length(P);
    if (tag_player(tag) || (!sky && (dist < sc.mb.z || dist < sc.mb.y)))
        return vec2(0.0);
    if (rider_motion(tag, v)) {
        v = v / sc.dims.zw * sc.mb.x;
    } else {
        v = vec2(0.0);
        vec4 pc = sc.prev_m * vec4(P, 1.0);
        if (pc.w > 1e-3)
            v = (p - pc.xy / pc.w / sc.dims.zw) * sc.mb.x;
        /* near snow streaks, the far backdrop much less (it barely moves) */
        v += (p - sc.mb3.xy / sc.dims.zw) * sc.mb3.z * clamp(sc.mb3.w / dist, 0.15, 1.0);
        vec2 e = p / sc.dims2.yz * 2.0 - 1.0;
        v *= 1.0 + sc.mb2.x * dot(e, e) * 0.5;
    }
    float len = length(v), mx = sc.mb2.y * scl;
    return len > mx ? v * (mx / len) : v;
}

float cone(float x, float len) { return clamp(1.0 - x / max(len, 1e-3), 0.0, 1.0); }
float cylinder(float x, float len) { return 1.0 - smoothstep(0.95 * len, 1.05 * len, x); }
/* 1 when a is in front of b, fading over 5% of the distance */
float in_front(float a, float b) { return clamp(1.0 - (a - b) / (0.05 * max(b, 1.0)), 0.0, 1.0); }

void main()
{
    scl = sc.dims2.z / 1080.0;
    vec2 p = gl_FragCoord.xy;
    vec4 src = texelFetch(img, ivec2(p), 0);
    float zc;
    vec2 v = blur_at(p, depth_at(ivec2(p)), src.a, zc);
    float lc = length(v);
    if (lc < 0.5) {
        o = vec4(src.rgb, src.a);
        return;
    }
    /* McGuire's weights: the centre counts as 1 / its blur length, each
     * sample by how much of it can reach (or be seen behind) the centre */
    float wc = 1.0 / lc;
    vec3 acc = src.rgb * wc;
    float wsum = wc, j = ign(p) - 0.5;
    for (int i = 0; i < TAPS; i++) {
        float t = (float(i) + 0.5 + j) / float(TAPS) - 0.5;
        if (abs(t) * lc < 0.5)
            continue;                                   /* the centre itself */
        vec2 q = clamp(p + v * t, vec2(0.5), sc.dims2.yz - 0.5);
        vec4 sq = texelFetch(img, ivec2(q), 0);
        if (tag_player(sq.a))
            continue;
        float zs;
        float ls = length(blur_at(q, depth_at(ivec2(q)), sq.a, zs));
        float x = length(q - p);
        float w = in_front(zs, zc) * cone(x, ls)       /* a nearer sample blurs over us */
                + in_front(zc, zs) * cone(x, lc)       /* we blur over what is behind */
                + cylinder(x, ls) * cylinder(x, lc) * 2.0;
        acc += textureLod(img, q / sc.dims2.yz, 0.0).rgb * w;
        wsum += w;
    }
    o = vec4(clamp(acc / wsum, 0.0, 1.0), src.a);
}
