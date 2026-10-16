/* Shared by scene_ao.frag and scene.frag (vkr.c scene_pass): the frame's
 * depth, and the camera that turns a pixel back into a position.
 *
 * Positions are relative to the eye, in the title's world units (SSX 3:
 * centimetres, Y up). inv_m maps (guest pixel x, y, depth - zmax, 1) to
 * them (homogeneous); it is the inverse of the terrain program's own
 * transform, measured on the CPU each frame (vkr_backend.c scene_camera).
 * Depth is used as (d - 1) * zmax: d is within 1% of 1 in a race, and the
 * difference is exact in float where d * zmax - zmax would not be. */

layout(set = 0, binding = 0) uniform sampler2D img;        /* the frame (with mips) */
#ifdef MS
layout(set = 0, binding = 1) uniform sampler2DMS dep;      /* depth, sample 0 */
#else
layout(set = 0, binding = 1) uniform sampler2D dep;
#endif
layout(set = 0, binding = 3) uniform sampler2D aot;        /* scene_ao.frag's result */

layout(set = 0, binding = 4, std140) uniform S {
    mat4 inv_m;         /* (gx, gy, d - zmax, 1) -> eye-relative position */
    mat4 prev_m;        /* eye-relative position -> last frame's (x w, y w, z w, w) */
    vec4 sun;           /* xyz: direction to the sun; w: how visible its disc is (0..1) */
    vec4 sun_scr;       /* xy: the sun in target pixels; zw: screen direction towards it */
    vec4 fog;           /* rgb: the haze colour (the event's fog colour) */
    vec4 ao;            /* strength, radius (world units), taps, min distance (nearer: HUD) */
    vec4 haze;          /* strength, start (world units), density (per unit); w: AO gain */
    vec4 fx;            /* shafts, motion blur, rim strength, rim range (world units) */
    vec4 dims;          /* guest w, h; guest pixels per target pixel x, y */
    vec4 dims2;         /* zmax, target w, h, motion blur near (world units) */
    vec4 dims3;         /* ao image w, h, time, - */
} sc;

ivec2 tsize() { return ivec2(sc.dims2.yz); }

float depth_at(ivec2 p)
{
    p = clamp(p, ivec2(0), tsize() - 1);
#ifdef MS
    return texelFetch(dep, p, 0).r;
#else
    return texelFetch(dep, p, 0).r;
#endif
}

/* p: target pixel (centre), d: its depth (0..1) */
vec3 pos_at(vec2 p, float d)
{
    vec4 q = sc.inv_m * vec4(p * sc.dims.zw, (d - 1.0) * sc.dims2.x, 1.0);
    return q.xyz / q.w;
}

bool is_sky(float d) { return d >= 1.0; }

/* The pass runs before the HUD (vkr_scene_run): everything drawn is world. */
bool is_3d(ivec2 p) { return true; }

float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

float hash12(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}
