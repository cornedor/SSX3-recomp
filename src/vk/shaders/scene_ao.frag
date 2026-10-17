#version 450
#extension GL_GOOGLE_include_directive : require

/* RECOMP_AO: ambient occlusion from the depth buffer, at half resolution
 * (vkr.c scene_pass; scene.frag blurs and applies it). Scalable Ambient
 * Obscurance style (McGuire et al. 2012): taps on a spiral around the pixel,
 * reconstructed into eye-relative positions, each occluding by how steeply
 * it rises above the surface's tangent plane, fading out with distance.
 *
 * Who darkens whom comes from the AO tags in the frame's alpha (xbox.frag
 * TAGS, vkr_backend.c ao_class): only the ground receives, only solid world
 * objects (rocks, trees, rails, signs, gates) occlude. So the snow darkens
 * where it meets them, while riders, trails, spray, thin beams and the
 * terrain's own folds are left alone. The spiral turns per pixel (4x4
 * pattern); scene.frag's blur averages the pattern out. Only darkens. */

#include "scene_common.glsl"

layout(location = 0) out vec4 o;

/* Pick the neighbour with the smaller depth step on each axis: no normals
 * smeared across silhouettes. */
vec3 normal_at(ivec2 ip, vec3 P)
{
    vec2 c = vec2(ip) + 0.5;
    vec3 px = pos_at(c + vec2(1, 0), depth_at(ip + ivec2(1, 0)));
    vec3 nx = pos_at(c - vec2(1, 0), depth_at(ip - ivec2(1, 0)));
    vec3 py = pos_at(c + vec2(0, 1), depth_at(ip + ivec2(0, 1)));
    vec3 ny = pos_at(c - vec2(0, 1), depth_at(ip - ivec2(0, 1)));
    vec3 dx = abs(px.z - P.z) + abs(px.y - P.y) + abs(px.x - P.x)
            < abs(nx.z - P.z) + abs(nx.y - P.y) + abs(nx.x - P.x) ? px - P : P - nx;
    vec3 dy = abs(py.z - P.z) + abs(py.y - P.y) + abs(py.x - P.x)
            < abs(ny.z - P.z) + abs(ny.y - P.y) + abs(ny.x - P.x) ? py - P : P - ny;
    vec3 n = normalize(cross(dx, dy));
    return dot(n, P) > 0.0 ? -n : n;            /* towards the eye */
}

float tag_at(ivec2 p) { return texelFetch(img, clamp(p, ivec2(0), tsize() - 1), 0).a; }

void main()
{
    ivec2 ip = ivec2(gl_FragCoord.xy * 2.0);
    float d = depth_at(ip);
    if (is_sky(d) || !tag_ground(tag_at(ip))) {
        o = vec4(1.0, is_sky(d) ? 1e9 : length(pos_at(vec2(ip) + 0.5, d)), 0.0, 1.0);
        return;
    }
    vec3 P = pos_at(vec2(ip) + 0.5, d);
    vec3 N = normal_at(ip, P);
    float R = sc.ao.y, R2 = R * R;
    /* the radius in target pixels at this distance: one pixel's world size */
    vec3 P1 = pos_at(vec2(ip) + vec2(1.5, 0.5), d);
    float rpx = clamp(R / max(length(P1 - P), 1e-4), 3.0, sc.dims2.z * 0.12);
    int taps = int(sc.ao.z);
    float rot = hash12(vec2(ip & 3) + 0.5) * 6.2831853;
    float sum = 0.0;
    for (int i = 0; i < taps; i++) {
        float a = (float(i) + 0.5) / float(taps);
        float ang = a * 6.2831853 * 2.6 + rot;      /* 2.6 turns: taps spread around */
        vec2 off = vec2(cos(ang), sin(ang)) * (a * rpx);
        ivec2 tp = ip + ivec2(off);
        float td = depth_at(tp);
        if (is_sky(td) || !tag_solid(tag_at(tp)))
            continue;
        vec3 v = pos_at(vec2(tp) + 0.5, td) - P;
        float vv = dot(v, v), vn = dot(v, N);
        /* above the tangent plane (beyond ~6 degrees: no self-darkening of
         * the snow under a rail lying on it), weighted down to the radius */
        float el = vn / sqrt(vv + 1e-4);
        float fall = max(1.0 - vv / R2, 0.0);
        sum += clamp((el - 0.1) / 0.9, 0.0, 1.0) * fall;
    }
    /* a soft curve that never reaches black: a deep corner under a rail or
     * an overhang stays shaded snow, not a hole */
    float ao = 1.0 - 0.7 * (1.0 - exp(-sum * sc.haze.w / float(taps)));
    o = vec4(ao, length(P), 0.0, 1.0);          /* .g: distance, for the blur's weights */
}
