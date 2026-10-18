#version 450

/* NV2A register combiners, interpreted per pixel. A straight port of
 * nv2a_combiner.c (xemu psh.c rules). SPEC = 0: the combiner words come from
 * the uniform buffer, so one pipeline serves every state. SPEC = 1: they are
 * specialization constants (vkr.c get_pipeline), the compiler folds the
 * interpreter away and R[] lives in registers -- on Apple's GPU the
 * interpreted version took ~80% of the frame's GPU time. */

layout(location = 0) in vec4 v_d0;
layout(location = 1) in vec4 v_d1;
layout(location = 2) in vec4 v_fog;
layout(location = 3) in vec4 v_tex0;
layout(location = 4) in vec4 v_tex1;
layout(location = 5) in vec4 v_tex2;
layout(location = 6) in vec4 v_tex3;

layout(location = 0) out vec4 out_color;
#ifdef TAGS
/* Rider tags (vkr.c get_pipeline): out_color.a carries the draw's tag for the
 * scene pass's motion blur -- 1 rider, 0 the rest -- and the blend equation
 * takes the real alpha from this second source instead. */
layout(location = 0, index = 1) out vec4 out_src1;
#endif

layout(set = 0, binding = 0) uniform sampler2D tex[4];

layout(set = 0, binding = 4, std140) uniform RC {
    uvec4 color_icw[2], alpha_icw[2], color_ocw[2], alpha_ocw[2];
    uvec4 factor0[2], factor1[2];
    uvec4 final_w;                  /* final0, final1, final_c0, final_c1 */
    uvec4 misc;                     /* control, stage_program, rc_seen, fog_enable */
    vec4 fog_color;
    vec4 tex_scale[4];
    vec4 fog_param;
} rc;

layout(push_constant) uniform PC {
    layout(offset = 64) uint alpha_func;  /* 0 = test off, else GL func 0x200..0x207 */
    uint alpha_ref;                       /* 0..255 */
    float w_scale;                        /* W-buffering: depth = w * this; 0 = off */
    uint point_sprite;                    /* stage 3 coordinates = gl_PointCoord (xemu psh.c) */
    uint tag;                             /* rider tag (TAGS): 1 = a rider */
} pc;

/* The specialized words: 0-7 colour ICW, 8-15 alpha ICW, 16-23 colour OCW,
 * 24-31 alpha OCW, 32 final0, 33 final1, 34 control, 35 stage program,
 * 36 combiners programmed, 37 alpha func. */
layout(constant_id = 0) const uint SPEC = 0u;
layout(constant_id = 1) const uint S0 = 0u;
layout(constant_id = 2) const uint S1 = 0u;
layout(constant_id = 3) const uint S2 = 0u;
layout(constant_id = 4) const uint S3 = 0u;
layout(constant_id = 5) const uint S4 = 0u;
layout(constant_id = 6) const uint S5 = 0u;
layout(constant_id = 7) const uint S6 = 0u;
layout(constant_id = 8) const uint S7 = 0u;
layout(constant_id = 9) const uint S8 = 0u;
layout(constant_id = 10) const uint S9 = 0u;
layout(constant_id = 11) const uint S10 = 0u;
layout(constant_id = 12) const uint S11 = 0u;
layout(constant_id = 13) const uint S12 = 0u;
layout(constant_id = 14) const uint S13 = 0u;
layout(constant_id = 15) const uint S14 = 0u;
layout(constant_id = 16) const uint S15 = 0u;
layout(constant_id = 17) const uint S16 = 0u;
layout(constant_id = 18) const uint S17 = 0u;
layout(constant_id = 19) const uint S18 = 0u;
layout(constant_id = 20) const uint S19 = 0u;
layout(constant_id = 21) const uint S20 = 0u;
layout(constant_id = 22) const uint S21 = 0u;
layout(constant_id = 23) const uint S22 = 0u;
layout(constant_id = 24) const uint S23 = 0u;
layout(constant_id = 25) const uint S24 = 0u;
layout(constant_id = 26) const uint S25 = 0u;
layout(constant_id = 27) const uint S26 = 0u;
layout(constant_id = 28) const uint S27 = 0u;
layout(constant_id = 29) const uint S28 = 0u;
layout(constant_id = 30) const uint S29 = 0u;
layout(constant_id = 31) const uint S30 = 0u;
layout(constant_id = 32) const uint S31 = 0u;
layout(constant_id = 33) const uint S32 = 0u;
layout(constant_id = 34) const uint S33 = 0u;
layout(constant_id = 35) const uint S34 = 0u;
layout(constant_id = 36) const uint S35 = 0u;
layout(constant_id = 37) const uint S36 = 0u;
layout(constant_id = 38) const uint S37 = 0u;
/* i is a constant wherever SPEC is set (the stages are unrolled). */
uint SW(uint i)
{
    switch (i) {
    case 0u: return S0;
    case 1u: return S1;
    case 2u: return S2;
    case 3u: return S3;
    case 4u: return S4;
    case 5u: return S5;
    case 6u: return S6;
    case 7u: return S7;
    case 8u: return S8;
    case 9u: return S9;
    case 10u: return S10;
    case 11u: return S11;
    case 12u: return S12;
    case 13u: return S13;
    case 14u: return S14;
    case 15u: return S15;
    case 16u: return S16;
    case 17u: return S17;
    case 18u: return S18;
    case 19u: return S19;
    case 20u: return S20;
    case 21u: return S21;
    case 22u: return S22;
    case 23u: return S23;
    case 24u: return S24;
    case 25u: return S25;
    case 26u: return S26;
    case 27u: return S27;
    case 28u: return S28;
    case 29u: return S29;
    case 30u: return S30;
    case 31u: return S31;
    case 32u: return S32;
    case 33u: return S33;
    case 34u: return S34;
    case 35u: return S35;
    case 36u: return S36;
    case 37u: return S37;
    default: return 0u;
    }
}

uint U(uvec4 a[2], uint i) { return a[i >> 2][i & 3u]; }
uint W(uint i, uint live) { return SPEC != 0u ? SW(i) : live; }
uint color_icw(uint s) { return W(s, U(rc.color_icw, s)); }
uint alpha_icw(uint s) { return W(8u + s, U(rc.alpha_icw, s)); }
uint color_ocw(uint s) { return W(16u + s, U(rc.color_ocw, s)); }
uint alpha_ocw(uint s) { return W(24u + s, U(rc.alpha_ocw, s)); }
uint final0()  { return W(32u, rc.final_w.x); }
uint final1()  { return W(33u, rc.final_w.y); }
uint control() { return W(34u, rc.misc.x); }
uint stage_program() { return W(35u, rc.misc.y); }
uint rc_seen() { return W(36u, rc.misc.z); }
uint alpha_func() { return W(37u, pc.alpha_func); }

vec4 R[16];

float map_in1(float x, uint byte)
{
    float p = max(x, 0.0);
    switch ((byte >> 5) & 7u) {
    case 0u: return p;
    case 1u: return 1.0 - clamp(x, 0.0, 1.0);
    case 2u: return 2.0 * p - 1.0;
    case 3u: return -2.0 * p + 1.0;
    case 4u: return p - 0.5;
    case 5u: return -p + 0.5;
    case 6u: return x;
    default: return -x;
    }
}

vec3 read_rgb(uint byte)
{
    vec4 r = R[byte & 15u];
    bool a = (byte & 0x10u) != 0u;
    return vec3(map_in1(a ? r.a : r.r, byte), map_in1(a ? r.a : r.g, byte),
                map_in1(a ? r.a : r.b, byte));
}

float read_alpha(uint byte)
{
    vec4 r = R[byte & 15u];
    return map_in1((byte & 0x10u) != 0u ? r.a : r.b, byte);
}

float map_out(float x, uint m)
{
    switch (m) {
    case 0x08u: return x - 0.5;
    case 0x10u: return x * 2.0;
    case 0x18u: return (x - 0.5) * 2.0;
    case 0x20u: return x * 4.0;
    case 0x30u: return x * 0.5;
    default:    return x;
    }
}
vec3 map_out3(vec3 x, uint m) { return vec3(map_out(x.x, m), map_out(x.y, m), map_out(x.z, m)); }

vec4 unpack_argb(uint c)
{
    return vec4(float((c >> 16) & 255u), float((c >> 8) & 255u), float(c & 255u),
                float(c >> 24)) / 255.0;
}

/* One stage's contribution by its SHADER_STAGE_PROGRAM mode (psh.c). */
vec4 stage_fetch(int st, vec4 c, out bool kill)
{
    uint mode = (stage_program() >> (uint(st) * 5u)) & 31u;
    float q = c.w != 0.0 ? c.w : 1.0;
    kill = false;
    if (mode == 0u)
        return vec4(0.0, 0.0, 0.0, 1.0);
    if (mode == 3u && rc.tex_scale[st].z > 0.5) {
        /* Cube map, its six faces stacked in one texture (+X -X +Y -Y +Z
         * -Z): pick the face by the major axis (nv2a_pb_exec.c, GL rules),
         * and keep v half a texel inside it so filtering stays on the face. */
        vec3 r = c.xyz, a = abs(r);
        float ma, sc, tc, face, h = rc.tex_scale[st].w;
        if (a.x >= a.y && a.x >= a.z) {
            face = r.x > 0.0 ? 0.0 : 1.0; ma = a.x; sc = r.x > 0.0 ? -r.z : r.z; tc = -r.y;
        } else if (a.y >= a.z) {
            face = r.y > 0.0 ? 2.0 : 3.0; ma = a.y; sc = r.x; tc = r.y > 0.0 ? r.z : -r.z;
        } else {
            face = r.z > 0.0 ? 4.0 : 5.0; ma = a.z; sc = r.z > 0.0 ? r.x : -r.x; tc = -r.y;
        }
        if (ma == 0.0)
            ma = 1.0;
        float u = (sc / ma + 1.0) * 0.5;
        float v = clamp((tc / ma + 1.0) * 0.5, 0.5 / h, 1.0 - 0.5 / h);
        return texture(tex[st], vec2(u, (face + v) / 6.0));
    }
    if (mode == 1u || mode == 2u || mode == 3u) {
        vec2 uv = (c.xy / q) * rc.tex_scale[st].xy;
        return texture(tex[st], uv);
    }
    if (mode == 4u)
        return clamp(c, 0.0, 1.0);
    return vec4(0.0);
}

/* One general combiner stage. */
void rc_stage(uint s, uint flags)
{
    uint icw = color_icw(s), ocw = color_ocw(s);
    uint aicw = alpha_icw(s), aocw = alpha_ocw(s);
    uint fl = ocw >> 12, afl = aocw >> 12;
    R[1] = unpack_argb(U(rc.factor0, (flags & 0x010u) != 0u ? s : 0u));
    R[2] = unpack_argb(U(rc.factor1, (flags & 0x100u) != 0u ? s : 0u));
    bool mux_cd = (flags & 1u) != 0u ? R[12].a >= 0.5
                                     : ((int(R[12].a * 255.0) & 1) != 0);

    vec3 A = read_rgb(icw >> 24), B = read_rgb(icw >> 16);
    vec3 C = read_rgb(icw >> 8),  D = read_rgb(icw);
    float aA = read_alpha(aicw >> 24), aB = read_alpha(aicw >> 16);
    float aC = read_alpha(aicw >> 8),  aD = read_alpha(aicw);

    float dab = dot(A, B), dcd = dot(C, D);
    vec3 pab = (fl & 2u) != 0u ? vec3(dab) : A * B;
    vec3 pcd = (fl & 1u) != 0u ? vec3(dcd) : C * D;
    vec3 sum = (fl & 4u) != 0u ? (mux_cd ? pcd : pab) : pab + pcd;
    vec3 ab = clamp(map_out3(pab, fl & 0x38u), -1.0, 1.0);
    vec3 cd = clamp(map_out3(pcd, fl & 0x38u), -1.0, 1.0);
    vec3 ms = clamp(map_out3(sum, fl & 0x38u), -1.0, 1.0);

    float apab = aA * aB, apcd = aC * aD;
    float asum = (afl & 4u) != 0u ? (mux_cd ? apcd : apab) : apab + apcd;
    float aab = clamp(map_out(apab, afl & 0x38u), -1.0, 1.0);
    float acd = clamp(map_out(apcd, afl & 0x38u), -1.0, 1.0);
    float ams = clamp(map_out(asum, afl & 0x38u), -1.0, 1.0);

    uint d1 = (ocw >> 4) & 15u, d2 = ocw & 15u, d3 = (ocw >> 8) & 15u;
    if (d1 != 0u) { R[d1].rgb = ab; if ((fl & 0x80u) != 0u) R[d1].a = ab.z; }
    if (d2 != 0u) { R[d2].rgb = cd; if ((fl & 0x40u) != 0u) R[d2].a = cd.z; }
    if (d3 != 0u) R[d3].rgb = ms;
    uint a1 = (aocw >> 4) & 15u, a2 = aocw & 15u, a3 = (aocw >> 8) & 15u;
    if (a1 != 0u) R[a1].a = aab;
    if (a2 != 0u) R[a2].a = acd;
    if (a3 != 0u) R[a3].a = ams;
    R[0] = vec4(0.0);
}

void main()
{
    uint n = min(control() & 255u, 8u), flags = control() >> 8;
    vec4 col;

    if (rc_seen() == 0u) {
        /* Combiners never programmed: stage 0 texel * diffuse. */
        col = v_d0;
        if ((stage_program() & 31u) != 0u)
            col *= texture(tex[0], v_tex0.xy * rc.tex_scale[0].xy);
    } else {
        vec4 t[4];
        bool kill;
        t[0] = stage_fetch(0, v_tex0, kill);
        t[1] = stage_fetch(1, v_tex1, kill);
        t[2] = stage_fetch(2, v_tex2, kill);
        t[3] = stage_fetch(3, pc.point_sprite != 0u ? vec4(gl_PointCoord, 1.0, 1.0) : v_tex3, kill);

        for (int i = 0; i < 16; i++)
            R[i] = vec4(0.0);
        R[3] = vec4(rc.fog_color.rgb, v_fog.x);
        R[4] = v_d0; R[5] = v_d1;
        R[8] = t[0]; R[9] = t[1]; R[10] = t[2]; R[11] = t[3];
        R[12].a = (stage_program() & 31u) != 0u ? t[0].a : 1.0;

        /* Unrolled by hand: with SPEC every stage index is a constant. */
        if (n > 0u) rc_stage(0u, flags);
        if (n > 1u) rc_stage(1u, flags);
        if (n > 2u) rc_stage(2u, flags);
        if (n > 3u) rc_stage(3u, flags);
        if (n > 4u) rc_stage(4u, flags);
        if (n > 5u) rc_stage(5u, flags);
        if (n > 6u) rc_stage(6u, flags);
        if (n > 7u) rc_stage(7u, flags);

        if (final0() != 0u || final1() != 0u) {
            uint f0 = final0(), f1 = final1(), ff = f1 & 255u;
            R[1] = unpack_argb(rc.final_w.z);
            R[2] = unpack_argb(rc.final_w.w);
            vec3 a = (ff & 0x40u) != 0u ? 1.0 - R[5].rgb : R[5].rgb;
            vec3 b = (ff & 0x20u) != 0u ? 1.0 - R[12].rgb : R[12].rgb;
            R[14] = vec4((ff & 0x80u) != 0u ? clamp(a + b, 0.0, 1.0) : a + b, 0.0);
            vec3 E = read_rgb(f1 >> 24), F = read_rgb(f1 >> 16);
            R[15] = vec4(E * F, 0.0);
            vec3 A = read_rgb(f0 >> 24), B = read_rgb(f0 >> 16);
            vec3 C = read_rgb(f0 >> 8),  D = read_rgb(f0);
            col = vec4(D + C * (1.0 - A) + B * A, read_alpha(f1 >> 8));
        } else {
            col = R[12];
        }
        col = clamp(col, 0.0, 1.0);
    }

    if (alpha_func() != 0u) {
        float a = floor(col.a * 255.0 + 0.5), r = float(pc.alpha_ref);
        bool pass;
        switch (alpha_func()) {
        case 0x200u: pass = false; break;
        case 0x201u: pass = a < r; break;
        case 0x202u: pass = a == r; break;
        case 0x203u: pass = a <= r; break;
        case 0x204u: pass = a > r; break;
        case 0x205u: pass = a != r; break;
        case 0x206u: pass = a >= r; break;
        default:     pass = true; break;
        }
        if (!pass)
            discard;
    }
    out_color = col;
#ifdef TAGS
    out_src1 = col;
    out_color.a = float(pc.tag);
#endif
#ifdef W_DEPTH
    /* CONTROL0 Z_PERSPECTIVE: the depth test compares w, which is linear in
     * distance, instead of z, whose precision is spent near the camera (the
     * race's terrain has z within 1% of the far end). Only this variant
     * (xbox_frag_wdepth, W-buffered draws) writes depth: a shader that does
     * turns off the GPU's early depth test, and every hidden pixel of every
     * draw was shaded -- a quarter of the GPU time at 5120x1440. */
    gl_FragDepth = clamp(pc.w_scale / gl_FragCoord.w, 0.0, 1.0);
#endif
}
