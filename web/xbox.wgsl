// The NV2A renderer's shaders for WebGPU: a port of src/vk/shaders (xbox.vert,
// xbox_vsh.vert, xbox.frag). Differences from the Vulkan ones:
//  - No vertex buffers: vertices (post-program from the CPU, or the program's
//    inputs for the GPU interpreter) and indices are read from the frame's
//    data buffer, and draws are non-indexed over expanded vertex counts.
//  - WebGPU draws points and lines one pixel wide, so they are expanded into
//    quads here (6 vertices per point or segment).
//  - WebGPU's clip space has y up (Vulkan's is down).
//  - No clamp-to-border sampler: a border stage samples transparent black
//    outside 0..1 (Draw.border).

struct Draw {
    color_icw: array<vec4u, 2>, alpha_icw: array<vec4u, 2>,
    color_ocw: array<vec4u, 2>, alpha_ocw: array<vec4u, 2>,
    factor0: array<vec4u, 2>, factor1: array<vec4u, 2>,
    final_w: vec4u,             // final0, final1, final_c0, final_c1
    misc: vec4u,                // control, stage_program, rc_seen, fog_enable
    fog_color: vec4f,
    tex_scale: array<vec4f, 4>,
    fog_param: vec4f,
    // vertex stage
    inv_size: vec2f, inv_zmax: f32, fog_mode: f32,
    fogp: vec2f, point_size: f32, point_params: f32,
    in_base: u32, nin: u32, prog_base: u32, const_base: u32,
    squeeze2d: f32, squeeze_all: f32, vsh: u32, idx_base: u32,
    vtx_base: u32, topo: u32, line_w: f32, alpha_func: u32,
    alpha_ref: u32, w_scale: f32, point_sprite: u32, border: u32,
    tsize: vec2f, ox: f32, pad: f32,
};

@group(0) @binding(0) var<uniform> P: Draw;
@group(0) @binding(1) var<storage, read> D: array<vec4u>;

@group(1) @binding(0) var t0: texture_2d<f32>;
@group(1) @binding(1) var t1: texture_2d<f32>;
@group(1) @binding(2) var t2: texture_2d<f32>;
@group(1) @binding(3) var t3: texture_2d<f32>;
@group(1) @binding(4) var s0: sampler;
@group(1) @binding(5) var s1: sampler;
@group(1) @binding(6) var s2: sampler;
@group(1) @binding(7) var s3: sampler;

struct VOut {
    @builtin(position) pos: vec4f,
    @location(0) d0: vec4f,
    @location(1) d1: vec4f,
    @location(2) fog: vec4f,
    @location(3) tex0: vec4f,
    @location(4) tex1: vec4f,
    @location(5) tex2: vec4f,
    @location(6) tex3: vec4f,
    @location(7) pcoord: vec2f,     // point sprite coordinate
    @location(8) cw: f32,           // clip w (W-buffered depth)
};

// One vertex after the vertex program, before the viewport mapping.
struct Vtx {
    pos: vec4f, d0: vec4f, d1: vec4f, fog: f32,
    t0: vec4f, t1: vec4f, t2: vec4f, t3: vec4f,
    psize: f32,
};

fn f4(i: u32) -> vec4f { return bitcast<vec4f>(D[i]); }

// An infinity (or any bit pattern) at run time: as a constant expression WGSL
// rejects a value f32 cannot represent.
fn fbits(b: u32) -> f32 { return bitcast<f32>(b | (P.topo & 0u)); }

fn index_at(i: u32) -> u32 {
    let w = P.idx_base + i;
    return D[w >> 2u][w & 3u];
}

fn fog_factor(d: f32) -> f32 {
    if (P.fog_mode == 0.0) { return 1.0; }
    let px = P.fogp.x;
    let py = P.fogp.y;
    let m = i32(P.fog_mode + 0.5);
    var f: f32;
    if (m == 0x800 || m == 0x802) {
        f = px + exp2(d * py * 16.0) - 1.5;
    } else if (m == 0x801 || m == 0x803) {
        f = px + exp2(-d * d * py * py * 32.0) - 1.5;
    } else {
        f = px + d * py - 1.0;
    }
    if (m == 0x802 || m == 0x803 || m == 0x804) { f = abs(f); }
    if (f != f) { f = 1.0; }
    return clamp(f, 0.0, 1.0);
}

// ---- the CPU path: VkrVertex, 8 x vec4 --------------------------------------

fn cpu_vertex(v: u32) -> Vtx {
    let b = P.vtx_base + v * 8u;
    var o: Vtx;
    o.pos = f4(b); o.d0 = f4(b + 1u); o.d1 = f4(b + 2u); o.fog = f4(b + 3u).x;
    o.t0 = f4(b + 4u); o.t1 = f4(b + 5u); o.t2 = f4(b + 6u); o.t3 = f4(b + 7u);
    o.psize = P.point_size;
    return o;
}

// ---- the GPU path: the NV2A vertex program, interpreted ---------------------
// A port of nv2a_vsh_run (toolkit nv2a_vsh_interp.c) in nv2a_vsh_export's
// format, as xbox_vsh.vert.

var<private> R: array<vec4f, 13>;   // R0..R11, R12 = oPos
var<private> O: array<vec4f, 13>;   // outputs by address
var<private> a0: i32;

fn src(w: u32, vbase: u32) -> vec4f {
    let mux = w & 3u;
    var s: vec4f;
    if (mux == 1u) {
        s = R[(w >> 2u) & 15u];
    } else if (mux == 2u) {
        s = f4(vbase + ((w >> 6u) & 15u));
    } else {
        var ci = i32((w >> 20u) & 255u);
        if (((w >> 11u) & 1u) != 0u) { ci = ci + a0; }
        if (ci < 0 || ci >= 192) { ci = 0; }
        s = f4(P.const_base + u32(ci));
    }
    s = vec4f(s[(w >> 12u) & 3u], s[(w >> 14u) & 3u], s[(w >> 16u) & 3u], s[(w >> 18u) & 3u]);
    if (((w >> 10u) & 1u) != 0u) { s = -s; }
    return s;
}

fn masked(d: vec4f, v: vec4f, m: u32) -> vec4f {
    var r = d;
    if ((m & 8u) != 0u) { r.x = v.x; }
    if ((m & 4u) != 0u) { r.y = v.y; }
    if ((m & 2u) != 0u) { r.z = v.z; }
    if ((m & 1u) != 0u) { r.w = v.w; }
    return r;
}

fn mac(op: u32, a: vec4f, b: vec4f, c: vec4f) -> vec4f {
    switch (op) {
        case 1u: { return a; }
        case 2u: { return a * b; }
        case 3u: { return a + c; }
        case 4u: { return a * b + c; }
        case 5u: { return vec4f(dot(a.xyz, b.xyz)); }
        case 6u: { return vec4f(dot(a.xyz, b.xyz) + b.w); }
        case 7u: { return vec4f(dot(a, b)); }
        case 8u: { return vec4f(1.0, a.y * b.y, a.z, b.w); }
        case 9u: { return min(a, b); }
        case 10u: { return select(b, a, a >= b); }
        case 11u: { return select(vec4f(0.0), vec4f(1.0), a < b); }
        case 12u: { return select(vec4f(0.0), vec4f(1.0), a >= b); }
        case 13u: { a0 = i32(floor(a.x + 0.001)); return a; }
        default: { return a; }
    }
}

fn ilu(op: u32, c: vec4f) -> vec4f {
    let x = c.x;
    switch (op) {
        case 1u: { return c; }
        case 2u: {
            if (x == 0.0) { return vec4f(fbits(0x7F800000u)); }
            return vec4f(1.0 / x);
        }
        case 3u: {
            var r = 1.0 / x;
            if (abs(r) < 5.42101e-20) { r = select(5.42101e-20, -5.42101e-20, r < 0.0); }
            if (abs(r) > 1.884467e19) { r = select(1.884467e19, -1.884467e19, r < 0.0); }
            return vec4f(r);
        }
        case 4u: { return vec4f(inverseSqrt(abs(x))); }
        case 5u: {
            let fl = floor(x);
            return vec4f(exp2(fl), x - fl, exp2(x), 1.0);
        }
        case 6u: {
            let ax = abs(x);
            if (ax == 0.0) {
                let ninf = fbits(0xFF800000u);
                return vec4f(ninf, 1.0, ninf, 1.0);
            }
            let e = floor(log2(ax));
            return vec4f(e, ax / exp2(e), log2(ax), 1.0);
        }
        case 7u: {
            let nl = max(c.x, 0.0);
            let nh = max(c.y, 0.0);
            let p = clamp(c.w, -127.9961, 127.9961);
            return vec4f(1.0, nl, select(0.0, pow(nh, p), c.x > 0.0), 1.0);
        }
        default: { return c; }
    }
}

fn vsh_vertex(v: u32) -> Vtx {
    let vbase = P.in_base + v * P.nin;
    for (var i = 0; i < 13; i++) {
        R[i] = vec4f(0.0);
        O[i] = vec4f(0.0);
    }
    O[3].w = 1.0; O[4].w = 1.0;
    O[9].w = 1.0; O[10].w = 1.0; O[11].w = 1.0; O[12].w = 1.0;
    a0 = 0;
    for (var s = 0u; s < 136u; s++) {
        let i0 = D[P.prog_base + 2u * s];
        let i1 = D[P.prog_base + 2u * s + 1u];
        let ctl = i0.x;
        let mop = ctl & 15u;
        let iop = (ctl >> 4u) & 7u;
        let mmask = (ctl >> 8u) & 15u;
        let imask = (ctl >> 12u) & 15u;
        let tdst = (ctl >> 16u) & 15u;
        let omask = (ctl >> 20u) & 15u;
        let a = src(i0.z, vbase);
        let b = src(i0.w, vbase);
        let c = src(i1.x, vbase);
        var mr = vec4f(0.0);
        var ir = vec4f(0.0);
        if (mop != 0u) { mr = mac(mop, a, b, c); }
        if (iop != 0u) { ir = ilu(iop, c); }
        if (mop != 0u && mop != 13u && mmask != 0u && tdst <= 12u) {
            R[tdst] = masked(R[tdst], mr, mmask);
        }
        if (iop != 0u && imask != 0u) {
            // Paired with a MAC op the ILU can only write R1.
            let it = select(tdst, 1u, mop != 0u);
            if (it <= 12u) { R[it] = masked(R[it], ir, imask); }
        }
        if (omask != 0u) {
            let val = select(mr, ir, ((ctl >> 24u) & 1u) != 0u);
            let oaddr = i0.y;
            if (((ctl >> 25u) & 1u) != 0u) {
                if (oaddr == 0u) {
                    R[12] = masked(R[12], val, omask);
                } else if (oaddr < 13u) {
                    O[oaddr] = masked(O[oaddr], val, omask);
                }
            }
        }
        if (((ctl >> 26u) & 1u) != 0u) { break; }
    }
    var o: Vtx;
    o.pos = R[12]; o.d0 = O[3]; o.d1 = O[4]; o.fog = O[5].x;
    o.t0 = O[9]; o.t1 = O[10]; o.t2 = O[11]; o.t3 = O[12];
    // The NV2A's point size tops out at 64 pixels (see xbox_vsh.vert).
    o.psize = select(P.point_size, clamp(O[6].x, 1.0, 64.0) * P.point_params, P.point_params > 0.0);
    return o;
}

fn get_vertex(v: u32) -> Vtx {
    if (P.vsh != 0u) { return vsh_vertex(v); }
    return cpu_vertex(v);
}

// Pixel position (guest pixels, z in depth units, clip w) -> clip space.
fn to_clip(p: vec4f) -> vec4f {
    let w = p.w;
    var ndc = p.xy * P.inv_size * 2.0 - 1.0;
    // 2D (w == 1): keep 4:3 around the centre, except full-screen quads.
    if (P.squeeze_all > 0.5 || (w == 1.0 && abs(ndc.x) < 0.999)) {
        ndc.x = ndc.x * P.squeeze2d;
    }
    return vec4f(ndc.x * w, -ndc.y * w, p.z * P.inv_zmax * w, w);
}

fn finish(v: Vtx, clip: vec4f) -> VOut {
    var o: VOut;
    o.pos = clip;
    // The NV2A clamps vertex colours to 0..1 before interpolation.
    o.d0 = clamp(v.d0, vec4f(0.0), vec4f(1.0));
    o.d1 = clamp(v.d1, vec4f(0.0), vec4f(1.0));
    o.fog = vec4f(fog_factor(v.fog), 0.0, 0.0, 0.0);
    o.tex0 = v.t0; o.tex1 = v.t1; o.tex2 = v.t2; o.tex3 = v.t3;
    o.pcoord = vec2f(0.0);
    o.cw = clip.w;
    return o;
}

var<private> CORNER: array<vec2f, 6> = array<vec2f, 6>(vec2f(-1.0, -1.0), vec2f(1.0, -1.0), vec2f(-1.0, 1.0),
                               vec2f(-1.0, 1.0), vec2f(1.0, -1.0), vec2f(1.0, 1.0));

@vertex
fn vs_main(@builtin(vertex_index) vi: u32) -> VOut {
    if (P.topo == 2u) {
        // Point: a size x size square around it; corner y runs down the screen.
        let v = get_vertex(index_at(vi / 6u));
        let k = CORNER[vi % 6u];
        var clip = to_clip(v.pos);
        let half = v.psize * 0.5;
        clip = vec4f(clip.x + k.x * half * 2.0 / P.tsize.x * clip.w,
                     clip.y - k.y * half * 2.0 / P.tsize.y * clip.w, clip.zw);
        var o = finish(v, clip);
        o.pcoord = k * 0.5 + 0.5;
        return o;
    }
    if (P.topo == 1u) {
        // Line segment: a quad line_w pixels wide along it.
        let seg = vi / 6u;
        let k = CORNER[vi % 6u];
        let va = get_vertex(index_at(seg * 2u));
        let vb = get_vertex(index_at(seg * 2u + 1u));
        let ca = to_clip(va.pos);
        let cb = to_clip(vb.pos);
        let sa = ca.xy / ca.w * P.tsize * 0.5;
        let sb = cb.xy / cb.w * P.tsize * 0.5;
        var dir = sb - sa;
        if (dot(dir, dir) < 1e-12) { dir = vec2f(1.0, 0.0); }
        let n = normalize(vec2f(-dir.y, dir.x)) * (P.line_w * 0.5) * k.y;
        let off = n / (P.tsize * 0.5);
        if (k.x < 0.0) {
            return finish(va, vec4f(ca.xy + off * ca.w, ca.zw));
        }
        return finish(vb, vec4f(cb.xy + off * cb.w, cb.zw));
    }
    let v = get_vertex(index_at(vi));
    return finish(v, to_clip(v.pos));
}

// ---- register combiners (xbox.frag) -------------------------------------------

var<private> C: array<vec4f, 16>;

fn U(a: array<vec4u, 2>, i: u32) -> u32 {
    var t = a;                      // runtime index: needs a memory view
    return t[i >> 2u][i & 3u];
}

fn map_in1(x: f32, b: u32) -> f32 {
    let p = max(x, 0.0);
    switch ((b >> 5u) & 7u) {
        case 0u: { return p; }
        case 1u: { return 1.0 - clamp(x, 0.0, 1.0); }
        case 2u: { return 2.0 * p - 1.0; }
        case 3u: { return -2.0 * p + 1.0; }
        case 4u: { return p - 0.5; }
        case 5u: { return -p + 0.5; }
        case 6u: { return x; }
        default: { return -x; }
    }
}

fn read_rgb(b: u32) -> vec3f {
    let r = C[b & 15u];
    if ((b & 0x10u) != 0u) {
        let a = map_in1(r.a, b);
        return vec3f(a);
    }
    return vec3f(map_in1(r.r, b), map_in1(r.g, b), map_in1(r.b, b));
}

fn read_alpha(b: u32) -> f32 {
    let r = C[b & 15u];
    return map_in1(select(r.b, r.a, (b & 0x10u) != 0u), b);
}

fn map_out(x: f32, m: u32) -> f32 {
    switch (m) {
        case 0x08u: { return x - 0.5; }
        case 0x10u: { return x * 2.0; }
        case 0x18u: { return (x - 0.5) * 2.0; }
        case 0x20u: { return x * 4.0; }
        case 0x30u: { return x * 0.5; }
        default: { return x; }
    }
}
fn map_out3(x: vec3f, m: u32) -> vec3f { return vec3f(map_out(x.x, m), map_out(x.y, m), map_out(x.z, m)); }

fn unpack_argb(c: u32) -> vec4f {
    return vec4f(f32((c >> 16u) & 255u), f32((c >> 8u) & 255u), f32(c & 255u), f32(c >> 24u)) / 255.0;
}

fn tsample(st: u32, uv: vec2f) -> vec4f {
    var r: vec4f;
    switch (st) {
        case 0u: { r = textureSampleLevel(t0, s0, uv, 0.0); }
        case 1u: { r = textureSampleLevel(t1, s1, uv, 0.0); }
        case 2u: { r = textureSampleLevel(t2, s2, uv, 0.0); }
        default: { r = textureSampleLevel(t3, s3, uv, 0.0); }
    }
    if (((P.border >> st) & 1u) != 0u && (any(uv < vec2f(0.0)) || any(uv > vec2f(1.0)))) {
        return vec4f(0.0);
    }
    return r;
}

// One stage's contribution by its SHADER_STAGE_PROGRAM mode (psh.c).
fn stage_fetch(st: u32, c: vec4f) -> vec4f {
    let mode = (P.misc.y >> (st * 5u)) & 31u;
    let q = select(1.0, c.w, c.w != 0.0);
    if (mode == 0u) { return vec4f(0.0, 0.0, 0.0, 1.0); }
    if (mode == 3u && P.tex_scale[st].z > 0.5) {
        // Cube map, six faces stacked (+X -X +Y -Y +Z -Z): pick the face.
        let r = c.xyz;
        let a = abs(r);
        var ma: f32; var sc: f32; var tc: f32; var face: f32;
        let h = P.tex_scale[st].w;
        if (a.x >= a.y && a.x >= a.z) {
            face = select(1.0, 0.0, r.x > 0.0); ma = a.x; sc = select(r.z, -r.z, r.x > 0.0); tc = -r.y;
        } else if (a.y >= a.z) {
            face = select(3.0, 2.0, r.y > 0.0); ma = a.y; sc = r.x; tc = select(-r.z, r.z, r.y > 0.0);
        } else {
            face = select(5.0, 4.0, r.z > 0.0); ma = a.z; sc = select(-r.x, r.x, r.z > 0.0); tc = -r.y;
        }
        if (ma == 0.0) { ma = 1.0; }
        let u = (sc / ma + 1.0) * 0.5;
        let v = clamp((tc / ma + 1.0) * 0.5, 0.5 / h, 1.0 - 0.5 / h);
        return tsample(st, vec2f(u, (face + v) / 6.0));
    }
    if (mode == 1u || mode == 2u || mode == 3u) {
        return tsample(st, (c.xy / q) * P.tex_scale[st].xy);
    }
    if (mode == 4u) { return clamp(c, vec4f(0.0), vec4f(1.0)); }
    return vec4f(0.0);
}

fn combine(i: VOut) -> vec4f {
    let n = min(P.misc.x & 255u, 8u);
    let flags = P.misc.x >> 8u;
    var col: vec4f;
    if (P.misc.z == 0u) {
        // Combiners never programmed: stage 0 texel * diffuse.
        col = i.d0;
        if ((P.misc.y & 31u) != 0u) {
            col = col * tsample(0u, i.tex0.xy * P.tex_scale[0].xy);
        }
        return col;
    }
    var t3c = i.tex3;
    if (P.point_sprite != 0u) { t3c = vec4f(i.pcoord, 1.0, 1.0); }
    let tx0 = stage_fetch(0u, i.tex0);
    let tx1 = stage_fetch(1u, i.tex1);
    let tx2 = stage_fetch(2u, i.tex2);
    let tx3 = stage_fetch(3u, t3c);
    for (var k = 0; k < 16; k++) { C[k] = vec4f(0.0); }
    C[3] = vec4f(P.fog_color.rgb, i.fog.x);
    C[4] = i.d0; C[5] = i.d1;
    C[8] = tx0; C[9] = tx1; C[10] = tx2; C[11] = tx3;
    C[12].a = select(1.0, tx0.a, (P.misc.y & 31u) != 0u);

    for (var s = 0u; s < n; s++) {
        let icw = U(P.color_icw, s);
        let ocw = U(P.color_ocw, s);
        let aicw = U(P.alpha_icw, s);
        let aocw = U(P.alpha_ocw, s);
        let fl = ocw >> 12u;
        let afl = aocw >> 12u;
        C[1] = unpack_argb(U(P.factor0, select(0u, s, (flags & 0x010u) != 0u)));
        C[2] = unpack_argb(U(P.factor1, select(0u, s, (flags & 0x100u) != 0u)));
        var mux_cd: bool;
        if ((flags & 1u) != 0u) { mux_cd = C[12].a >= 0.5; }
        else { mux_cd = (i32(C[12].a * 255.0) & 1) != 0; }

        let A = read_rgb(icw >> 24u);
        let B = read_rgb(icw >> 16u);
        let Cc = read_rgb(icw >> 8u);
        let Dd = read_rgb(icw);
        let aA = read_alpha(aicw >> 24u);
        let aB = read_alpha(aicw >> 16u);
        let aC = read_alpha(aicw >> 8u);
        let aD = read_alpha(aicw);

        let dab = dot(A, B);
        let dcd = dot(Cc, Dd);
        let pab = select(A * B, vec3f(dab), (fl & 2u) != 0u);
        let pcd = select(Cc * Dd, vec3f(dcd), (fl & 1u) != 0u);
        let sum = select(pab + pcd, select(pab, pcd, mux_cd), (fl & 4u) != 0u);
        let ab = clamp(map_out3(pab, fl & 0x38u), vec3f(-1.0), vec3f(1.0));
        let cd = clamp(map_out3(pcd, fl & 0x38u), vec3f(-1.0), vec3f(1.0));
        let ms = clamp(map_out3(sum, fl & 0x38u), vec3f(-1.0), vec3f(1.0));

        let apab = aA * aB;
        let apcd = aC * aD;
        let asum = select(apab + apcd, select(apab, apcd, mux_cd), (afl & 4u) != 0u);
        let aab = clamp(map_out(apab, afl & 0x38u), -1.0, 1.0);
        let acd = clamp(map_out(apcd, afl & 0x38u), -1.0, 1.0);
        let ams = clamp(map_out(asum, afl & 0x38u), -1.0, 1.0);

        let d1 = (ocw >> 4u) & 15u;
        let d2 = ocw & 15u;
        let d3 = (ocw >> 8u) & 15u;
        if (d1 != 0u) { C[d1] = vec4f(ab, select(C[d1].a, ab.z, (fl & 0x80u) != 0u)); }
        if (d2 != 0u) { C[d2] = vec4f(cd, select(C[d2].a, cd.z, (fl & 0x40u) != 0u)); }
        if (d3 != 0u) { C[d3] = vec4f(ms, C[d3].a); }
        let a1 = (aocw >> 4u) & 15u;
        let a2 = aocw & 15u;
        let a3 = (aocw >> 8u) & 15u;
        if (a1 != 0u) { C[a1].a = aab; }
        if (a2 != 0u) { C[a2].a = acd; }
        if (a3 != 0u) { C[a3].a = ams; }
        C[0] = vec4f(0.0);
    }

    if (P.final_w.x != 0u || P.final_w.y != 0u) {
        let f0 = P.final_w.x;
        let f1 = P.final_w.y;
        let ff = f1 & 255u;
        C[1] = unpack_argb(P.final_w.z);
        C[2] = unpack_argb(P.final_w.w);
        let a = select(C[5].rgb, 1.0 - C[5].rgb, (ff & 0x40u) != 0u);
        let b = select(C[12].rgb, 1.0 - C[12].rgb, (ff & 0x20u) != 0u);
        C[14] = vec4f(select(a + b, clamp(a + b, vec3f(0.0), vec3f(1.0)), (ff & 0x80u) != 0u), 0.0);
        let E = read_rgb(f1 >> 24u);
        let F = read_rgb(f1 >> 16u);
        C[15] = vec4f(E * F, 0.0);
        let A = read_rgb(f0 >> 24u);
        let B = read_rgb(f0 >> 16u);
        let Cc = read_rgb(f0 >> 8u);
        let Dd = read_rgb(f0);
        col = vec4f(Dd + Cc * (1.0 - A) + B * A, read_alpha(f1 >> 8u));
    } else {
        col = C[12];
    }
    return clamp(col, vec4f(0.0), vec4f(1.0));
}

fn alpha_pass(a_in: f32) -> bool {
    if (P.alpha_func == 0u) { return true; }
    let a = floor(a_in * 255.0 + 0.5);
    let r = f32(P.alpha_ref);
    switch (P.alpha_func) {
        case 0x200u: { return false; }
        case 0x201u: { return a < r; }
        case 0x202u: { return a == r; }
        case 0x203u: { return a <= r; }
        case 0x204u: { return a > r; }
        case 0x205u: { return a != r; }
        case 0x206u: { return a >= r; }
        default: { return true; }
    }
}

@fragment
fn fs_main(i: VOut) -> @location(0) vec4f {
    let col = combine(i);
    if (!alpha_pass(col.a)) { discard; }
    return col;
}

struct FOutDepth {
    @location(0) color: vec4f,
    @builtin(frag_depth) depth: f32,
};

// W-buffered draws (CONTROL0 Z_PERSPECTIVE): depth is clip w scaled to 0..1.
// Only this variant writes depth -- a shader that does loses early depth tests.
@fragment
fn fs_main_wdepth(i: VOut) -> FOutDepth {
    let col = combine(i);
    if (!alpha_pass(col.a)) { discard; }
    var o: FOutDepth;
    o.color = col;
    o.depth = clamp(P.w_scale * i.cw, 0.0, 1.0);
    return o;
}

// ---- clears: a full-target triangle inside the scissor ----------------------

struct ClearOut { @builtin(position) pos: vec4f };

@vertex
fn vs_clear(@builtin(vertex_index) vi: u32) -> ClearOut {
    var p = array<vec2f, 3>(vec2f(-1.0, -1.0), vec2f(3.0, -1.0), vec2f(-1.0, 3.0));
    var o: ClearOut;
    o.pos = vec4f(p[vi], 0.0, 1.0);
    return o;
}

struct ClearFrag {
    @location(0) color: vec4f,
    @builtin(frag_depth) depth: f32,
};

@fragment
fn fs_clear(i: ClearOut) -> ClearFrag {
    var o: ClearFrag;
    o.color = P.fog_color;          // the clear colour
    o.depth = P.fog_param.x;        // the clear depth
    return o;
}
