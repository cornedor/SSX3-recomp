#version 450

/* CRT monitor post-process (RECOMP_CRT, from rs-board's glfx.rs): the frame
 * re-emitted as a curved tube with Gaussian scanline beams, slot mask,
 * misconvergence and halation. The picture is the rectangle p.rect of the
 * target (the 4:3 middle when pillarboxed); outside the tube is black. */

layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D frame;   /* with mips: halation */

layout(push_constant) uniform P {
    vec4 rect;          /* picture x, y, w, h in target pixels */
    vec2 tsize;         /* frame texture size */
    float curv;         /* barrel strength of the glass */
    float scan;         /* beam sharpness: 0 flat, 1 thin beams */
    float mask;         /* slot mask strength */
    float vig;          /* edge darkening */
    float ab;           /* RGB misconvergence */
    float glow;         /* halation */
    float bright;
    float time;         /* seconds */
    float mpitch;       /* slot-mask triad width in target pixels */
    float lines;        /* scanlines over the picture height */
    float flicker;      /* hum bar + brightness jitter */
    float noise;        /* grain */
} p;

/* Picture uv -> frame, at an explicit mip level, in (gamma 2) linear light. */
vec3 fetch(vec2 uv, float lod)
{
    vec2 c = clamp(uv, 0.5 / p.rect.zw, 1.0 - 0.5 / p.rect.zw);
    vec3 t = textureLod(frame, (p.rect.xy + c * p.rect.zw) / p.tsize, lod).rgb;
    return t * t;
}

/* One scanline's colour at x, box-filtered over the slot by two taps. */
vec3 line(float x, float y, float hx, vec2 conv)
{
    vec3 c = fetch(vec2(x - hx, y), 0.0) + fetch(vec2(x + hx, y), 0.0);
    if (p.ab > 0.0) {
        c.r = fetch(vec2(x + conv.x - hx, y + conv.y), 0.0).r
            + fetch(vec2(x + conv.x + hx, y + conv.y), 0.0).r;
        c.b = fetch(vec2(x - conv.x - hx, y - conv.y), 0.0).b
            + fetch(vec2(x - conv.x + hx, y - conv.y), 0.0).b;
    }
    return c * 0.5;
}

float hash(vec2 q)
{
    return fract(sin(dot(q, vec2(12.9898, 78.233))) * 43758.5453);
}

/* Each axis bows with the square of the other: edges curve, corners pull in. */
vec2 curve(vec2 q)
{
    vec2 cc = q * 2.0 - 1.0;
    cc *= 1.0 + p.curv * 0.5 * (cc.yx * cc.yx);
    return cc * 0.5 + 0.5;
}

void main()
{
    vec2 res = p.rect.zw;
    vec2 uv = curve((gl_FragCoord.xy - p.rect.xy) / res);
    vec2 cc = uv * 2.0 - 1.0;
    /* Rounded, anti-aliased picture edge (distance in target pixels). */
    float rad = min(res.x, res.y) * 0.035;
    vec2 q = abs(uv - 0.5) * res - (0.5 * res - rad);
    float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - rad;
    float edge = clamp(0.5 - d / 1.5, 0.0, 1.0);
    if (edge <= 0.0) {
        out_color = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    /* Slot mask on the curved glass: RGB triads mpitch wide and tall, every
     * other column shifted half a cell down, rounded stripes with gaps. */
    float pitch = max(p.mpitch, 1.0);
    vec2 lp = uv * res;
    float slot_w = pitch / 3.0;
    float col_i = floor(lp.x / pitch);
    float k = floor((lp.x - col_i * pitch) / slot_w);
    float sy = lp.y / pitch + 0.5 * mod(col_i, 2.0);
    vec2 sp = vec2(lp.x - col_i * pitch - (k + 0.5) * slot_w, (fract(sy) - 0.5) * pitch);
    vec2 half_sz = 0.5 * vec2(slot_w * 0.75, pitch * 0.82);
    float sr = half_sz.x * 0.6;
    vec2 sq = abs(sp) - half_sz + sr;
    float sd = length(max(sq, 0.0)) + min(max(sq.x, sq.y), 0.0) - sr;
    float cov = clamp(0.5 - sd / max(slot_w * 0.3, 1.0), 0.0, 1.0);
    vec3 sel = clamp(1.0 - abs(vec3(k) - vec3(0.0, 1.0, 2.0)), 0.0, 1.0);
    vec3 mk = sel * cov * 3.0 / (0.75 * 0.82 * 0.95);
    /* A slot glows evenly: sample the picture at the slot's centre column. */
    float sx = (col_i * pitch + (k + 0.5) * slot_w) / res.x;
    float ux = mix(uv.x, sx, p.mask);
    float hx = max(slot_w * 0.25, 0.5) / res.x;
    float lpitch = res.y / max(p.lines, 1.0);
    vec2 conv = (cc * 0.8 + vec2(0.2, 0.0)) * p.ab * lpitch * 0.25 / res;
    /* Electron beams: one sample per scanline, each a Gaussian that widens
     * with brightness; only the two nearest lines reach this pixel. */
    float ly = uv.y * p.lines - 0.5;
    float base = floor(ly);
    float f = ly - base;
    float s_dark = mix(0.55, 0.15, p.scan);
    float s_bright = mix(0.55, 0.38, p.scan);
    vec3 c0 = line(ux, (base + 0.5) / p.lines, hx, conv);
    vec3 c1 = line(ux, (base + 1.5) / p.lines, hx, conv);
    vec3 g0 = mix(vec3(s_dark), vec3(s_bright), sqrt(c0));
    vec3 g1 = mix(vec3(s_dark), vec3(s_bright), sqrt(c1));
    vec3 w0 = exp(-0.5 * f * f / (g0 * g0));
    vec3 w1 = exp(-0.5 * (1.0 - f) * (1.0 - f) / (g1 * g1));
    vec3 acc = c0 * w0 + c1 * w1;
    float comp = 1.0 / (mix(s_dark, s_bright, 0.5) * 2.5066);
    vec3 col = mix(acc / max(w0 + w1, vec3(0.0001)), acc * comp, smoothstep(0.0, 0.25, p.scan));
    col *= mix(vec3(1.0), mk, p.mask);
    /* Halation: light scattered in the glass. */
    if (p.glow > 0.0) {
        float hl = log2(max(lpitch, 1.0));
        vec3 halo = fetch(uv, hl + 3.0) * 0.6 + fetch(uv, hl + 5.0) * 0.4;
        col += halo * p.glow * 0.2;
    }
    float v = clamp(16.0 * uv.x * uv.y * (1.0 - uv.x) * (1.0 - uv.y), 0.0, 1.0);
    col *= pow(v, p.vig * 0.4);
    float hum = sin(6.2831853 * (uv.y * 0.6 - p.time * 0.12));
    float jit = hash(vec2(floor(p.time * 50.0), 3.7)) - 0.5;
    col *= 1.0 + p.flicker * (hum + jit);
    col = sqrt(max(col * p.bright, vec3(0.0)));
    vec2 g = floor(lp / pitch);
    col += (hash(g + fract(p.time * 13.37) * vec2(113.0, 271.0)) - 0.5) * p.noise * 0.08;
    out_color = vec4(col * edge, 1.0);
}
