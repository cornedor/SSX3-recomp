#version 450
#extension GL_GOOGLE_include_directive : require

/* The scene pass (vkr.c scene_pass): effects that need to know where a pixel
 * is in the world, on the finished 3D image before the HUD is drawn over it
 * (vkr_scene_run). Each is its own flag, 0 = off:
 *   RECOMP_MBLUR    camera motion blur: each pixel reprojected with last
 *                   frame's camera, blurred along the difference; not near
 *                   the camera (the rider rides with it)
 *   RECOMP_AO       ambient occlusion (scene_ao.frag), blurred here
 *   RECOMP_RIM      a thin sun-side rim on near silhouettes (the riders)
 *   RECOMP_HAZE     aerial perspective: distance fades towards the event's
 *                   fog colour
 *   RECOMP_SHAFTS   light shafts from the bright sky towards the sun */

#include "scene_common.glsl"

layout(location = 0) out vec4 o;

vec3 frame(vec2 p) { return textureLod(img, p / sc.dims2.yz, 0.0).rgb; }

void main()
{
    ivec2 ip = ivec2(gl_FragCoord.xy);
    vec4 src = texelFetch(img, ip, 0);
    vec3 col = src.rgb;
    float d = depth_at(ip);
    bool sky = is_sky(d), s3d = is_3d(ip) && !sky;
    /* nearer than ao.w: HUD models (the trick meter) drawn in 3D close to
     * the camera, not the world */
    if (s3d && length(pos_at(gl_FragCoord.xy, d)) < sc.ao.w)
        s3d = false;
    float scl = sc.dims2.z / 1080.0;                /* pixel sizes are for 1080 lines */

    if (s3d) {
        vec3 P = pos_at(gl_FragCoord.xy, d);
        float dist = length(P);

        if (sc.fx.y > 0.0 && dist > sc.dims2.w) {
            vec4 pc = sc.prev_m * vec4(P, 1.0);
            if (pc.w > 1e-3) {
                vec2 prev = pc.xy / pc.w / sc.dims.zw;          /* target pixels */
                vec2 vel = (gl_FragCoord.xy - prev) * sc.fx.y;
                float len = length(vel), mx = 48.0 * scl;
                if (len > mx) vel *= mx / len;
                /* fade in past the near distance: no seam at the rider */
                vel *= smoothstep(sc.dims2.w, sc.dims2.w * 2.0, dist);
                if (length(vel) > 0.5) {
                    vec3 acc = vec3(0.0);
                    for (int i = 0; i < 8; i++)
                        acc += frame(gl_FragCoord.xy + vel * ((float(i) + 0.5) / 8.0 - 0.5));
                    col = acc / 8.0;
                }
            }
        }

        if (sc.ao.x > 0.0) {
            /* 4x4 of the half-resolution result (the whole 4x4 rotation
             * pattern), weighted by depth likeness: the pattern averages
             * out, edges stay */
            vec2 auv = gl_FragCoord.xy * 0.5 - 0.5;
            float wsum = 0.0, a = 0.0;
            for (int j = -1; j <= 2; j++)
                for (int i = -1; i <= 2; i++) {
                    ivec2 q = clamp(ivec2(floor(auv)) + ivec2(i, j), ivec2(0), ivec2(sc.dims3.xy) - 1);
                    vec2 t = texelFetch(aot, q, 0).rg;
                    float w = 1.0 / (1e-3 + abs(t.y - dist) / max(dist, 1.0) * 50.0);
                    a += t.x * w;
                    wsum += w;
                }
            a /= wsum;
            col *= mix(1.0, a, sc.ao.x);
        }

        if (sc.fx.z > 0.0 && dist < sc.fx.w) {
            /* a silhouette facing the sun on screen: the pixel a few
             * pixels sunwards is sky or much farther */
            /* a silhouette, not a receding surface: the step towards the
             * sun must be much bigger than the step away from it */
            ivec2 st = ivec2(round(sc.sun_scr.zw * 2.0 * max(scl, 0.5)));
            ivec2 q = ip + st, b = ip - st;
            float qd = depth_at(q), bd = depth_at(b);
            float edge = 0.0;
            if (!is_sky(bd) && is_3d(b)) {
                float bdist = length(pos_at(vec2(b) + 0.5, bd));
                float qdist = is_sky(qd) || !is_3d(q) ? dist * 4.0 : length(pos_at(vec2(q) + 0.5, qd));
                edge = smoothstep(0.25, 0.6, ((qdist - dist) - abs(dist - bdist)) / dist);
            }
            float k = sc.fx.z * edge * (1.0 - smoothstep(sc.fx.w * 0.5, sc.fx.w, dist));
            col += k * vec3(1.0, 0.97, 0.9) * (1.0 - 0.5 * col);
        }

        if (sc.haze.x > 0.0) {
            float h = sc.haze.x * (1.0 - exp(-max(dist - sc.haze.y, 0.0) * sc.haze.z));
            col = mix(col, sc.fog.rgb, h);
        }
    }

    if (sc.fx.x > 0.0 && sc.sun.w > 0.0 && (sky || s3d)) {
        /* radial samples from here towards the sun of what is bright sky
         * (GPU Gems 3, ch. 13); the mountains and the riders block it */
        vec2 toward = sc.sun_scr.xy - gl_FragCoord.xy;
        float acc = 0.0, wt = 1.0, wsum = 0.0;
        vec3 lc = vec3(0.0);
        float j = hash12(gl_FragCoord.xy + sc.dims3.z) * 0.9;
        for (int i = 0; i < 24; i++) {
            vec2 p = gl_FragCoord.xy + toward * ((float(i) + j) / 24.0) * 0.85;
            ivec2 q = ivec2(p);
            if (any(lessThan(q, ivec2(0))) || any(greaterThanEqual(q, tsize())))
                break;
            if (is_sky(depth_at(q))) {
                vec3 c = textureLod(img, p / sc.dims2.yz, 2.0).rgb;
                float b = max(luma(c) - 0.6, 0.0) * 2.5;
                lc += c * b * wt;
            }
            wsum += wt;
            wt *= 0.95;
        }
        if (wsum > 0.0) {
            /* strongest near the sun's direction on screen */
            float near = 1.0 - smoothstep(0.0, 0.9, length(toward) / sc.dims2.z);
            col += sc.fx.x * sc.sun.w * near * lc / wsum * (1.0 - 0.6 * col);
        }
    }
    if (sc.dims3.w > 0.5) {
        vec3 dc = vec3(0.0);
        if (s3d) {
            vec3 P = pos_at(gl_FragCoord.xy, d);
            if (sc.dims3.w < 1.5)
                dc = vec3(texelFetch(aot, ivec2(gl_FragCoord.xy * 0.5), 0).r);
            else if (sc.dims3.w < 2.5)
                dc = normalize(cross(dFdx(P), dFdy(P))) * 0.5 + 0.5;
            else
                dc = vec3(fract(length(P) / 1000.0), length(P) / 20000.0, 0.0);
        }
        col = dc;
    }
    o = vec4(clamp(col, 0.0, 1.0), src.a);
}
