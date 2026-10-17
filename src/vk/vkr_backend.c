/* Glue: the executor's Nv2aBackend on top of the Vulkan renderer.
 *
 * Milestone-2 scope: one "main" colour target (the largest non-swizzled
 * 32-bit surface drawn to). Draws and clears on any other target are declined
 * so the software rasteriser still handles them in guest memory. */
#include "vkr_thread.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#define access _access
#define F_OK 0
#else
#include <unistd.h>
#endif

/* RECOMP_VKR_STATS=1: where the backend's time goes, once a second. */
static double s_t[6];            /* draw total, lookup+hash, decode+upload, vkr_draw, flip */
static unsigned s_n[3];          /* draws, texture creates, flips */
static int s_stats = -1;

static double now_ms(void)
{
#ifdef _WIN32
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart)
        QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1e3 / (double)f.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
#endif
}

static void stats_tick(void)
{
    static double last;
    double t = now_ms();
    if (!last) last = t;
    if (t - last < 1000.0)
        return;
    fprintf(stderr, "[VKRSTAT] draws %u tex-creates %u flips %u | ms/s: draw-total %.0f lookup+hash %.0f"
            " decode+upload %.0f vkr_draw %.0f flip %.0f\n", s_n[0], s_n[1], s_n[2],
            s_t[0], s_t[1], s_t[2], s_t[3], s_t[4]);
    memset(s_t, 0, sizeof s_t); memset(s_n, 0, sizeof s_n);
    last = t;
}

#include "nv2a_backend.h"
#include "vkr.h"
#include "vkr_backend.h"

extern ptrdiff_t xbox_GetMemoryOffset(void);

/* RECOMP_AB=<dir>,<start>[,<pairs>[,<interval ms>]]: A/B pairs at full
 * resolution, <dir>/ab<k>_on.ppm (as configured) and ab<k>_off.ppm (the
 * per-frame remaster features off: vkr_ab_off), a few frames apart; start is
 * ms after the first flip, or "b<ms>" after the first race briefing (the
 * race start: the same camera in every run). RECOMP_AB_KEEP=1: both shots
 * as configured (to compare two runs). ab_flip asks, the flip thread writes
 * the next frame. */
static char s_ab_path[512];
static volatile int s_ab_req;

static void ab_capture(const uint8_t *bgra, uint32_t w, uint32_t h)
{
    FILE *f;
    uint8_t *line;
    uint32_t x, y;
    if (!__atomic_load_n(&s_ab_req, __ATOMIC_ACQUIRE))
        return;
    f = fopen(s_ab_path, "wb");
    line = (uint8_t *)malloc((size_t)w * 3);
    if (f && line) {
        fprintf(f, "P6\n%u %u\n255\n", w, h);
        for (y = 0; y < h; y++) {
            const uint8_t *src = bgra + (size_t)y * w * 4;
            for (x = 0; x < w; x++) {
                line[x * 3] = src[x * 4 + 2]; line[x * 3 + 1] = src[x * 4 + 1];
                line[x * 3 + 2] = src[x * 4];
            }
            fwrite(line, 1, (size_t)w * 3, f);
        }
        fprintf(stderr, "[AB] wrote %s (%ux%u)\n", s_ab_path, w, h);
    }
    if (f) fclose(f);
    free(line);
    __atomic_store_n(&s_ab_req, 0, __ATOMIC_RELEASE);
}
extern void xbox_FramebufferPublishImage(const uint32_t *px, uint32_t w, uint32_t h);
extern int ssx3_WidescreenNow(void);
extern float ssx3_Aspect(void);
extern void ssx3_SetAspect(float a);
extern int xbox_FramebufferClientSize(int *w, int *h);

_Static_assert(sizeof(VkrVertex) == sizeof(Nv2aVshOutput),
               "VkrVertex must mirror Nv2aVshOutput");

#define PRIM_POINTS         1
#define PRIM_LINES          2
#define PRIM_LINE_LOOP      3
#define PRIM_LINE_STRIP     4
#define PRIM_TRIANGLES      5
#define PRIM_TRIANGLE_STRIP 6
#define PRIM_TRIANGLE_FAN   7
#define PRIM_QUADS          8
#define PRIM_QUAD_STRIP     9
#define PRIM_POLYGON        10

static VkrContext *s_vk;
static uint32_t s_w, s_h, s_zmax;
static int s_dirty;                     /* drawn since the last flip */

/* Flipped frames on their way to guest memory. The executor submits a frame
 * and goes on with the next one; this thread waits for the GPU and copies
 * the result into the surface the title flipped, so the executor never
 * waits for the GPU to finish a frame (it did: ~5 ms a flip, a quarter of
 * its time in a race). The copy lands a few ms after the flip. */
typedef struct { int slot; uint32_t offset, pitch, x, y, w, h; } FlipJob;
static FlipJob s_jobs[4];
static int s_job_head, s_job_n;
static pthread_mutex_t s_job_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_job_cond = PTHREAD_COND_INITIALIZER;

/* For the slow-frame catcher (ssx3_overrides.c, RECOMP_SLOW_STACKS): when
 * the last flip came and on which threads (the GPU executor, the
 * flip thread that reads frames back). */
volatile uint32_t ssx3_last_flip_ms, ssx3_exec_tid, ssx3_flip_tid;
extern uint32_t xbox_TickCountMs(void);

static void *flip_thread(void *unused)
{
    (void)unused;
#ifdef _WIN32
    ssx3_flip_tid = (uint32_t)GetCurrentThreadId();
#endif
    for (;;) {
        FlipJob j;
        const uint8_t *px;
        uint32_t y;
        pthread_mutex_lock(&s_job_lock);
        while (!s_job_n)
            pthread_cond_wait(&s_job_cond, &s_job_lock);
        j = s_jobs[s_job_head];
        pthread_mutex_unlock(&s_job_lock);
        px = vkr_frame_pixels(s_vk, j.slot);
        if (px) {
            uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
            uint32_t fw, fh;
            const uint8_t *full;
            for (y = 0; y < j.h; y++)
                memcpy(mem + j.offset + (size_t)(j.y + y) * j.pitch + (size_t)j.x * 4,
                       px + (size_t)y * j.w * 4, (size_t)j.w * 4);
            /* Higher internal resolution: the window shows the full image. */
            full = vkr_frame_pixels_full(s_vk, j.slot, &fw, &fh);
            if (full) {
                ab_capture(full, fw, fh);
                xbox_FramebufferPublishImage((const uint32_t *)full, fw, fh);
            }
        }
        pthread_mutex_lock(&s_job_lock);
        s_job_head = (s_job_head + 1) % 4;
        s_job_n--;
        pthread_mutex_unlock(&s_job_lock);
        vkr_frame_release(s_vk, j.slot);
    }
    return NULL;
}

/* Is t the surface the Vulkan target stands for? Adopts the first one that
 * looks like a frame buffer (>= 320x240 clip, linear, 32-bit). The target
 * covers the surface from its origin -- vertex positions are relative to it
 * -- and only grows: the clip rectangle (360..480 lines in a race: the
 * letterboxed start camera, HUD passes) becomes the scissor. Sizing the
 * target by the clip recreated it, blank, mid-frame and mapped positions
 * with the wrong size: black and misplaced geometry in races. */
/* Surfaces that have been flipped to the screen (double/triple buffered):
 * only those are the Vulkan target. Another linear 32-bit surface the title
 * renders into -- an offscreen image it later samples as a texture -- was
 * drawn into the main image instead: its memory stayed black (the black
 * shapes in races) and the frame got draws meant for it. Until the first
 * flip any frame-buffer-like surface is taken. */
static uint32_t s_front[4];
static int s_nfront;

static int is_front(uint32_t off)
{
    int i;
    for (i = 0; i < s_nfront; i++)
        if (s_front[i] == off)
            return 1;
    return s_nfront == 0;
}

/* Render targets the title draws (swizzled: the shadow maps), see note_rt. */
static uint32_t s_rt[16];
static int s_nrt;
static struct { uint32_t off, w, h; VkrTex *tex; } s_rt_tex[16];
static int s_nrt_tex;

/* Swizzled render targets the title draws (rider shadow maps): drawn in
 * software; RECOMP_VKR_NO_SHADOW=1 skips the draws that sample them (an
 * experiment for the black shadow-receiver shapes). */
static void note_rt(uint32_t off)
{
    int i;
    for (i = 0; i < s_nrt; i++)
        if (s_rt[i] == off)
            return;
    if (s_nrt < 16)
        s_rt[s_nrt++] = off;
}

static int is_rt(uint32_t off)
{
    int i;
    for (i = 0; i < s_nrt; i++)
        if (s_rt[i] == off)
            return 1;
    return 0;
}

static int samples_rt(const VkrDrawState *st)
{
    int k, i;
    for (k = 0; k < 4; k++) {
        Nv2aTexInfo ti;
        if (!((st->u.stage_program >> (k * 5)) & 0x1F) || !nv2a_exec_tex_info(k, &ti))
            continue;
        for (i = 0; i < s_nrt; i++)
            if (s_rt[i] == ti.offset)
                return 1;
    }
    return 0;
}

static int is_main(const Nv2aTarget *t)
{
    uint32_t w, h;
    if (t->swizzled)
        note_rt(t->color_offset);
    if (t->swizzled || t->bpp != 4)
        return 0;
    if (!is_front(t->color_offset)) {
        static uint32_t said[8];
        int i;
        for (i = 0; i < 8 && said[i] && said[i] != t->color_offset; i++)
            ;
        if (i < 8 && !said[i]) {
            said[i] = t->color_offset;
            fprintf(stderr, "[VKR] offscreen surface %08X %ux%u: drawn in software\n",
                    t->color_offset, t->width, t->height);
        }
        return 0;
    }
    if (t->width < 320 || t->height < 240)
        return 0;
    if (t->x + t->width > t->pitch / 4)
        return 0;
    w = t->x + t->width > s_w ? t->x + t->width : s_w;
    h = t->y + t->height > s_h ? t->y + t->height : s_h;
    if (w != s_w || h != s_h || t->zmax != s_zmax) {
        if (!vkr_set_target(s_vk, w, h, (float)t->zmax))
            return 0;
        s_w = w; s_h = h; s_zmax = t->zmax;
        fprintf(stderr, "[VKR] target %ux%u zmax %X\n", s_w, s_h, s_zmax);
    }
    vkr_set_scissor(s_vk, t->x, t->y, t->width, t->height);
    return 1;
}

/* RECOMP_VKR_GPU_RT (default on; 0 = the software rasteriser draws them into
 * guest memory as before): swizzled 32-bit render targets -- SSX 3's rider
 * shadow maps -- are drawn on the GPU at RECOMP_VKR_RT_SCALE times their size
 * and sampled from there. The surface's size is the next power of two of
 * the clip's far corner (the maps are 128x128, clip 2,2 124x124). */
static int s_gpu_rt = -1;

static uint32_t pow2_at_least(uint32_t v)
{
    uint32_t p = 1;
    while (p < v && p < 4096)
        p <<= 1;
    return p;
}

static int gpu_rt_on(void)
{
    if (s_gpu_rt < 0) {
        const char *e = getenv("RECOMP_VKR_GPU_RT");
        s_gpu_rt = !(e && *e == '0');
    }
    return s_gpu_rt;
}

/* Point the renderer at t: the main target or a GPU render target. 0: not
 * ours (the software rasteriser draws it). */
static unsigned s_frame_draw_no, s_frame_draws_last;   /* main-target draws per frame (RECOMP_SCENE_TRACE) */

static int select_target(const Nv2aTarget *t)
{
    /* The title clears its shadow maps through a pitch-linear surface at
     * the same address (format 0x07070128): a known render target is that
     * target whatever the layout says. */
    if ((t->swizzled || is_rt(t->color_offset)) && t->bpp == 4 && gpu_rt_on()) {
        uint32_t w = pow2_at_least(t->x + t->width), h = pow2_at_least(t->y + t->height);
        note_rt(t->color_offset);
        if (!vkr_rt_bind(s_vk, t->color_offset, w, h, (float)(t->zmax ? t->zmax : 0xFFFF)))
            return 0;
        vkr_set_scissor(s_vk, t->x, t->y, t->width, t->height);
        {
            static int dbg = -1, n;
            if (dbg < 0) dbg = getenv("RECOMP_RT_DEBUG") != NULL;
            if (dbg && (n < 12 || !t->swizzled) && n < 40) {
                n++;
                fprintf(stderr, "[RTDBG] %s into %08X clip %u,%u %ux%u blend %d %X/%X mask %08X alpha %d/%X/%u"
                        " depth %d/%d final %08X %08X rc %u\n", t->swizzled ? "draw" : "LINEAR", t->color_offset, t->x, t->y, t->width,
                        t->height, t->blend_enable, t->blend_sfactor, t->blend_dfactor, t->color_mask,
                        t->alpha_test, t->alpha_func, t->alpha_ref, t->depth_test, t->depth_write,
                        t->rc.final0, t->rc.final1, t->rc_seen);
            }
        }
        return 1;
    }
    vkr_use_main(s_vk);
    if (!is_main(t))
        return 0;
    s_frame_draw_no++;
    return 1;
}

static void fill_state(const Nv2aTarget *t, VkrDrawState *st)
{
    vkr_set_pillarbox(s_vk, !ssx3_WidescreenNow());
    VkrUniforms *u = &st->u;
    int k;
    st->point_size = t->point_size;
    st->point_params = t->point_params;
    st->point_sprite = t->point_sprite;
    st->depth_test = t->depth_test; st->depth_write = t->depth_write;
    st->depth_func = t->depth_func;
    st->stencil_test = t->stencil_test;
    st->stencil_func = t->stencil_func; st->stencil_ref = t->stencil_ref;
    st->stencil_read_mask = t->stencil_read_mask; st->stencil_write_mask = t->stencil_write_mask;
    st->stencil_fail = t->stencil_fail; st->stencil_zfail = t->stencil_zfail;
    st->stencil_zpass = t->stencil_zpass;
    st->w_buffer = t->w_buffer;
    st->blend_enable = t->blend_enable;
    st->blend_src = t->blend_sfactor; st->blend_dst = t->blend_dfactor;
    st->blend_eq = t->blend_equation;
    st->blend_color[0] = (float)((t->blend_color >> 16) & 0xFF) / 255.0f;
    st->blend_color[1] = (float)((t->blend_color >> 8) & 0xFF) / 255.0f;
    st->blend_color[2] = (float)(t->blend_color & 0xFF) / 255.0f;
    st->blend_color[3] = (float)(t->blend_color >> 24) / 255.0f;
    st->write_mask = ((t->color_mask >> 16) & 0xFF ? 1u : 0u) | ((t->color_mask >> 8) & 0xFF ? 2u : 0u)
                   | (t->color_mask & 0xFF ? 4u : 0u) | (t->color_mask >> 24 ? 8u : 0u);
    st->alpha_func = t->alpha_test ? t->alpha_func : 0;
    st->alpha_ref = t->alpha_ref & 0xFF;

    memcpy(u->color_icw, t->rc.color_icw, sizeof u->color_icw);
    memcpy(u->alpha_icw, t->rc.alpha_icw, sizeof u->alpha_icw);
    memcpy(u->color_ocw, t->rc.color_ocw, sizeof u->color_ocw);
    memcpy(u->alpha_ocw, t->rc.alpha_ocw, sizeof u->alpha_ocw);
    memcpy(u->factor0, t->rc.factor0, sizeof u->factor0);
    memcpy(u->factor1, t->rc.factor1, sizeof u->factor1);
    u->final0 = t->rc.final0; u->final1 = t->rc.final1;
    u->final_c0 = t->rc.final_c0; u->final_c1 = t->rc.final_c1;
    u->control = t->rc.control; u->stage_program = t->rc.stage_program;
    u->rc_seen = (uint32_t)t->rc_seen;
    u->fog_enable = t->fog_enable;
    {
        static int no_fog = -1;         /* RECOMP_VKR_NO_FOG=1: experiment */
        if (no_fog < 0) no_fog = getenv("RECOMP_VKR_NO_FOG") != NULL;
        if (no_fog) u->fog_enable = 0;
    }
    /* FOG_COLOR is R in bits 0-7, the reverse of a D3DCOLOR. */
    u->fog_color[0] = (float)(t->fog_color & 0xFF) / 255.0f;
    u->fog_color[1] = (float)((t->fog_color >> 8) & 0xFF) / 255.0f;
    u->fog_color[2] = (float)((t->fog_color >> 16) & 0xFF) / 255.0f;
    u->fog_color[3] = (float)(t->fog_color >> 24) / 255.0f;
    u->fog_param[0] = t->fog_param[0]; u->fog_param[1] = t->fog_param[1];
    u->fog_param[2] = (float)t->fog_mode;
    for (k = 0; k < 4; k++) {
        u->tex_scale[k][0] = u->tex_scale[k][1] = 1.0f;
        u->tex_scale[k][2] = u->tex_scale[k][3] = 0.0f;
        st->addr_u[k] = st->addr_v[k] = 3;
    }
}


/* RECOMP_DEBLOCK=<threshold, 0..255; 0 = off>: DXT textures are made of
 * 4x4 blocks with four colours each; magnified (the mountain backdrops are
 * stretched 2-3x at 1080p) the block seams show as a grid. Across every
 * block border, a step smaller than the threshold -- with smooth texels on
 * both sides -- is a seam, not an edge, and is smoothed over the two texels
 * each side; larger steps (real edges) stay sharp. Colour only; the alpha
 * of cut-out textures is untouched. Once per decoded texture. */
static int s_deblock = -1;

static void deblock_line(uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d, int thr)
{
    uint32_t na = *a, nb = *b, nc = *c, nd = *d;
    int sh;
    for (sh = 0; sh < 24; sh += 8) {
        int p1 = (int)(*a >> sh) & 255, p0 = (int)(*b >> sh) & 255;
        int q0 = (int)(*c >> sh) & 255, q1 = (int)(*d >> sh) & 255;
        int step = q0 - p0;
        if (abs(step) >= thr || abs(p1 - p0) * 2 >= thr || abs(q1 - q0) * 2 >= thr)
            return;                     /* an edge (in any channel): keep the texels */
        (void)p1; (void)q1;
    }
    for (sh = 0; sh < 24; sh += 8) {
        int p1 = (int)(*a >> sh) & 255, p0 = (int)(*b >> sh) & 255;
        int q0 = (int)(*c >> sh) & 255, q1 = (int)(*d >> sh) & 255;
        /* a ramp across the four: p1 p0 | q0 q1 */
        int np0 = (p1 + 2 * p0 + q0 + 2) >> 2, nq0 = (p0 + 2 * q0 + q1 + 2) >> 2;
        int np1 = (3 * p1 + p0 + 2) >> 2, nq1 = (3 * q1 + q0 + 2) >> 2;
        na = (na & ~(255u << sh)) | (uint32_t)np1 << sh;
        nb = (nb & ~(255u << sh)) | (uint32_t)np0 << sh;
        nc = (nc & ~(255u << sh)) | (uint32_t)nq0 << sh;
        nd = (nd & ~(255u << sh)) | (uint32_t)nq1 << sh;
    }
    *a = na; *b = nb; *c = nc; *d = nd;
}

static void deblock(uint32_t *px, uint32_t w, uint32_t h, int thr)
{
    uint32_t x, y;
    for (y = 0; y < h; y++)
        for (x = 4; x + 1 < w; x += 4) {
            uint32_t *r = px + (size_t)y * w;
            deblock_line(&r[x - 2], &r[x - 1], &r[x], &r[x + 1], thr);
        }
    for (y = 4; y + 1 < h; y += 4)
        for (x = 0; x < w; x++)
            deblock_line(&px[(size_t)(y - 2) * w + x], &px[(size_t)(y - 1) * w + x],
                         &px[(size_t)y * w + x], &px[(size_t)(y + 1) * w + x], thr);
}

/* Bind the texture stages the combiners use. */
static void bind_textures(VkrDrawState *st)
{
    int k;
    {
        /* RECOMP_VKR_STAGES=1: each distinct stage program once, with which
         * stages hold a cube map -- what the title's materials use. */
        static int on = -1, n;
        static uint32_t seen[64];
        int i;
        if (on < 0)
            on = getenv("RECOMP_VKR_STAGES") != NULL;
        if (on && n < 64) {
            uint32_t key = st->u.stage_program;
            for (k = 0; k < 4; k++) {
                Nv2aTexInfo ti;
                if (((key >> (k * 5)) & 0x1F) && nv2a_exec_tex_info(k, &ti) && ti.cube)
                    key |= 0x80000000u >> k;
            }
            for (i = 0; i < n && seen[i] != key; i++)
                ;
            if (i == n) {
                seen[n++] = key;
                fprintf(stderr, "[VK-STAGES] program %05X cube stages %X final %08X %08X\n",
                        key & 0xFFFFF, key >> 28, st->u.final0, st->u.final1);
            }
        }
    }
    {
        static uint32_t gen;                /* the texture cache was purged: drop our pointers */
        if (vkr_tex_generation(s_vk) != gen) {
            gen = vkr_tex_generation(s_vk);
            s_nrt_tex = 0;
        }
    }
    for (k = 0; k < 4; k++) {
        Nv2aTexInfo ti;
        VkrTex *tex;
        double t0 = s_stats ? now_ms() : 0;
        if (!((st->u.stage_program >> (k * 5)) & 0x1F))
            continue;
        if (!nv2a_exec_tex_info(k, &ti))
            continue;
        tex = NULL;
        if (gpu_rt_on() && is_rt(ti.offset) && !ti.cube) {
            tex = vkr_rt_texture(s_vk, ti.offset, ti.width, ti.height);
            {
                /* RECOMP_RT_DEBUG=1: how the title samples its render
                 * targets (first few draws) */
                static int dbg = -1, n;
                if (dbg < 0) dbg = getenv("RECOMP_RT_DEBUG") != NULL;
                if (dbg && n < 6) {
                    int i;
                    n++;
                    fprintf(stderr, "[RTDBG] stage %d off %08X fmt %02X %ux%u addr %u/%u lin %d prog %05X"
                            " rc %u ctl %08X final %08X %08X alpha %X/%u blend %d %X/%X mask %X\n",
                            k, ti.offset, ti.format, ti.width, ti.height, ti.addr_u, ti.addr_v,
                            ti.linear_filter, st->u.stage_program, st->u.rc_seen, st->u.control,
                            st->u.final0, st->u.final1, st->alpha_func, st->alpha_ref, st->blend_enable,
                            st->blend_src, st->blend_dst, st->write_mask);
                    for (i = 0; i < (int)(st->u.control & 0xFF) && i < 8; i++)
                        fprintf(stderr, "[RTDBG]   rc%d icw %08X ocw %08X aicw %08X aocw %08X f0 %08X f1 %08X\n",
                                i, st->u.color_icw[i], st->u.color_ocw[i], st->u.alpha_icw[i],
                                st->u.alpha_ocw[i], st->u.factor0[i], st->u.factor1[i]);
                }
            }
        }
        if (!tex)
            tex = vkr_tex_find(s_vk, ti.hash);
        if (s_stats) s_t[1] += now_ms() - t0;
        if (!tex) {
            double t1 = s_stats ? now_ms() : 0;
            uint32_t rows = ti.cube ? ti.height * 6 : ti.height;  /* cube: faces stacked */
            uint32_t *px = (uint32_t *)malloc((size_t)ti.width * rows * 4);
            int rt = is_rt(ti.offset), j;
            if (px && nv2a_exec_tex_decode(k, px)) {
                if (s_deblock < 0) {
                    const char *e = getenv("RECOMP_DEBLOCK");
                    s_deblock = e ? atoi(e) : 0;
                }
                if (s_deblock > 0 && !rt && !ti.cube && ti.width >= 16 && ti.height >= 16
                    && (ti.format == 0x0C || ti.format == 0x0E || ti.format == 0x0F))
                    deblock(px, ti.width, ti.height, s_deblock);
                /* A render target redrawn every frame: one texture per
                 * address, updated in place, not a new one per frame. */
                VkrTex *old = NULL;
                if (rt)
                    for (j = 0; j < s_nrt_tex; j++)
                        if (s_rt_tex[j].off == ti.offset && s_rt_tex[j].w == ti.width
                            && s_rt_tex[j].h == ti.height)
                            old = s_rt_tex[j].tex;
                if (old && vkr_tex_update(s_vk, old, ti.hash, px))
                    tex = old;
                else {
                    tex = vkr_tex_create_ex(s_vk, ti.hash, ti.width, rows, px, !rt && !ti.cube);
                    if (tex && rt && s_nrt_tex < 16) {
                        s_rt_tex[s_nrt_tex].off = ti.offset; s_rt_tex[s_nrt_tex].w = ti.width;
                        s_rt_tex[s_nrt_tex].h = ti.height; s_rt_tex[s_nrt_tex].tex = tex;
                        s_nrt_tex++;
                    }
                }
            }
            free(px);
            if (s_stats) { s_t[2] += now_ms() - t1; s_n[1]++; }
        }
        st->tex[k] = tex;
        st->addr_u[k] = ti.cube ? 3 : ti.addr_u;       /* a cube's faces are clamped */
        st->addr_v[k] = ti.cube ? 3 : ti.addr_v;
        st->linear[k] = ti.linear_filter;
        st->u.tex_scale[k][0] = ti.normalised ? 1.0f : 1.0f / (float)ti.width;
        st->u.tex_scale[k][1] = ti.normalised ? 1.0f : 1.0f / (float)ti.height;
        st->u.tex_scale[k][2] = ti.cube ? 1.0f : 0.0f;
        st->u.tex_scale[k][3] = (float)ti.height;
    }
}

/* RECOMP_VKR_TRACE_AT=<ms since start>: log every large draw (> 2000 px of
 * screen box) of the first frame after that time -- what a wrong-looking
 * region is drawn with. s_trace: 0 off/done, 1 armed, 2 this frame. */
static int s_trace = -1;
static double s_trace_at, s_t0;
/* RECOMP_VKR_TRACE_EVERY=<ms>: re-arm after each traced frame, so a run
 * traces one frame per period (vkr_trace_<k>.bmp, log lines tagged k). */
static double s_trace_every;
static double s_trace_brief = -1;
extern volatile int g_ssx3_at_briefing;     /* autonav.c */
static unsigned s_trace_no;

static void trace_draw(const Nv2aTarget *t, uint32_t prim, const Nv2aVshOutput *v, uint32_t n,
                       const VkrDrawState *st)
{
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f, z0 = 1e30f, z1 = -1e30f;
    uint32_t i;
    int k;
    for (i = 0; i < n; i++) {
        if (v[i].pos[2] < z0) z0 = v[i].pos[2];
        if (v[i].pos[2] > z1) z1 = v[i].pos[2];
        if (v[i].pos[0] < x0) x0 = v[i].pos[0];
        if (v[i].pos[0] > x1) x1 = v[i].pos[0];
        if (v[i].pos[1] < y0) y0 = v[i].pos[1];
        if (v[i].pos[1] > y1) y1 = v[i].pos[1];
    }
    if ((x1 - x0) * (y1 - y0) < 2000.0f)
        return;
    fprintf(stderr, "[VKRTRACE] prim %u n %u box %.0f,%.0f-%.0f,%.0f z %.4g..%.4g w0 %.3g d0 %.2f,%.2f,%.2f,%.2f | stages %05X",
            prim, n, x0, y0, x1, y1, z0 / (t->zmax ? (float)t->zmax : 1.0f), z1 / (t->zmax ? (float)t->zmax : 1.0f),
            v[0].pos[3], v[0].d0[0], v[0].d0[1], v[0].d0[2], v[0].d0[3],
            st->u.stage_program);
    for (k = 0; k < 4; k++) {
        Nv2aTexInfo ti;
        if (!((st->u.stage_program >> (k * 5)) & 0x1F))
            continue;
        if (nv2a_exec_tex_info(k, &ti)) {
            const uint32_t *m = (const uint32_t *)((const uint8_t *)xbox_GetMemoryOffset() + ti.offset);
            fprintf(stderr, " t%d:%08X fmt %X %ux%u%s [%08X %08X %08X %08X]", k, ti.offset, ti.format,
                    ti.width, ti.height, st->tex[k] ? "" : " (none)", m[0], m[1], m[2], m[3]);
        }
        else
            fprintf(stderr, " t%d:invalid", k);
    }
    fprintf(stderr, " | rc %u final %08X %08X fog %u | blend %d %X/%X alpha %X ref %u mask %X depth %d/%d %X\n",
            st->u.rc_seen, st->u.final0, st->u.final1, st->u.fog_enable, st->blend_enable, st->blend_src,
            st->blend_dst, st->alpha_func, st->alpha_ref, st->write_mask, st->depth_test, st->depth_write,
            st->depth_func);
    (void)t;
}

/* RECOMP_VKR_TRACE_BLACK=<n> (with RECOMP_VKR_TRACE_AT): in the traced frame,
 * read the target back after each of the first n draws and log how many
 * pixels are pure black after it, with the draw's vertices -- which draw
 * paints, or fails to cover, a black region. A readback per draw; n < 0
 * logs the first -n draws without reading back. */
static int s_tblack, s_tblack_init;
static unsigned s_draw_no;
static FILE *s_tbf;
static uint32_t *s_tbpx;   /* its own readback buffer, s_w x s_h */

static long count_black(void)
{
    long nb = 0;
    uint32_t i;
    for (i = 0; i < s_w * s_h; i++)
        if (!(s_tbpx[i] & 0xFFFFFF))
            nb++;
    return nb;
}

static void trace_black(uint32_t prim, const Nv2aVshOutput *v, uint32_t n, const VkrDrawState *st)
{
    uint32_t i;
    if (!s_tblack_init) {
        const char *e = getenv("RECOMP_VKR_TRACE_BLACK");
        s_tblack_init = 1;
        s_tblack = e ? atoi(e) : 0;
        /* Its own buffered file: thousands of lines through stderr took
         * seconds per draw. */
        if (s_tblack && !(s_tbf = fopen("vkr_black.txt", "w")))
            s_tblack = 0;
        if (s_tblack > 0 && !(s_tbpx = (uint32_t *)malloc((size_t)s_w * s_h * 4)))
            s_tblack = 0;
    }
    if (s_tblack == 0)
        return;
    /* A negative n: no readbacks, every draw's vertices (black count -1). */
    if (++s_draw_no > (unsigned)abs(s_tblack))
        return;
    if (s_tblack > 0 && !vkr_read_back(s_vk, (uint8_t *)s_tbpx))
        return;
    fprintf(s_tbf, "[VKRBLACK] %u draw #%u black %ld prim %u n %u stages %05X final %08X %08X"
            " blend %d %X/%X alpha %X/%u depth %d/%d %X fog %u\n", s_trace_no, s_draw_no,
            s_tblack > 0 ? count_black() : -1L,
            prim, n, st->u.stage_program, st->u.final0, st->u.final1, st->blend_enable, st->blend_src,
            st->blend_dst, st->alpha_func, st->alpha_ref, st->depth_test, st->depth_write,
            st->depth_func, st->u.fog_enable);
    {
        Nv2aTexInfo ti;
        if ((st->u.stage_program & 0x1F) && nv2a_exec_tex_info(0, &ti))
            fprintf(s_tbf, "[VKRBLACK]   t0 %08X fmt %X %ux%u addr %u/%u\n", ti.offset, ti.format,
                    ti.width, ti.height, ti.addr_u, ti.addr_v);
    }
    {
        /* RECOMP_VKR_TRACE_VSH=<a>-<b>: draws a..b of the traced frame also
         * log their program, constants and vertex inputs. */
        static int lo = -1, hi;
        if (lo < 0) {
            const char *e = getenv("RECOMP_VKR_TRACE_VSH");
            lo = hi = 0;
            if (e) sscanf(e, "%d-%d", &lo, &hi);
        }
        if ((int)s_draw_no >= lo && (int)s_draw_no <= hi)
            nv2a_exec_dump_batch(s_tbf, 64);
    }
    for (i = 0; i < n && i < 512; i++)
        fprintf(s_tbf, "[VKRBLACK]   v%u pos %g %g %g %g d0 %.2f %.2f %.2f %.2f fog %g t0 %g %g %g %g\n",
                i, v[i].pos[0], v[i].pos[1], v[i].pos[2], v[i].pos[3], v[i].d0[0], v[i].d0[1],
                v[i].d0[2], v[i].d0[3], v[i].fog[0], v[i].tex[0][0], v[i].tex[0][1],
                v[i].tex[0][2], v[i].tex[0][3]);
}

/* Triangle-list indices for n vertices of primitive prim, in a buffer
 * reused across calls; *k gets the count. NULL: not drawable here (points,
 * lines) or out of memory. */
/* Index list for one NV2A primitive; *topo: 0 triangle list, 1 line list,
 * 2 point list (VKR_TOPO_*). */
static uint32_t *build_idx(uint32_t prim, uint32_t n, uint32_t *k_out, uint32_t *topo)
{
    static uint32_t *idx;
    static uint32_t cap;
    uint32_t i, k = 0, need = n * 6;
    if (cap < need) {
        free(idx);
        cap = need;
        idx = (uint32_t *)malloc(cap * sizeof *idx);
        if (!idx) { cap = 0; return NULL; }
    }
    *topo = VKR_TOPO_TRIANGLES;
    switch (prim) {
    case PRIM_POINTS:
        *topo = VKR_TOPO_POINTS;
        for (i = 0; i < n; i++)
            idx[k++] = i;
        break;
    case PRIM_LINES:
        *topo = VKR_TOPO_LINES;
        for (i = 0; i + 1 < n; i += 2) {
            idx[k++] = i; idx[k++] = i + 1;
        }
        break;
    case PRIM_LINE_STRIP:
    case PRIM_LINE_LOOP:
        *topo = VKR_TOPO_LINES;
        for (i = 0; i + 1 < n; i++) {
            idx[k++] = i; idx[k++] = i + 1;
        }
        if (prim == PRIM_LINE_LOOP && n > 2) {
            idx[k++] = n - 1; idx[k++] = 0;
        }
        break;
    case PRIM_TRIANGLES:
        for (i = 0; i + 2 < n; i += 3) {
            idx[k++] = i; idx[k++] = i + 1; idx[k++] = i + 2;
        }
        break;
    case PRIM_TRIANGLE_STRIP:
        for (i = 0; i + 2 < n; i++) {
            /* keep the winding consistent; culling is off, order is free */
            idx[k++] = i; idx[k++] = i + 1; idx[k++] = i + 2;
        }
        break;
    case PRIM_TRIANGLE_FAN:
    case PRIM_POLYGON:
        for (i = 1; i + 1 < n; i++) {
            idx[k++] = 0; idx[k++] = i; idx[k++] = i + 1;
        }
        break;
    case PRIM_QUADS:
        for (i = 0; i + 3 < n; i += 4) {
            idx[k++] = i; idx[k++] = i + 1; idx[k++] = i + 2;
            idx[k++] = i; idx[k++] = i + 2; idx[k++] = i + 3;
        }
        break;
    case PRIM_QUAD_STRIP:
        for (i = 0; i + 3 < n; i += 2) {
            idx[k++] = i; idx[k++] = i + 1; idx[k++] = i + 3;
            idx[k++] = i; idx[k++] = i + 3; idx[k++] = i + 2;
        }
        break;
    default:
        {   /* anything else: say so once per type */
            static uint32_t said;
            if (prim < 32 && !(said & (1u << prim))) {
                said |= 1u << prim;
                fprintf(stderr, "[VKR] primitive %u (%u vertices) not drawn on the GPU\n", prim, n);
                fflush(stderr);
            }
        }
        return NULL;
    }
    *k_out = k;
    return idx;
}


/* RECOMP_VKR_PRIMLOG=1: each distinct state a line or point batch is drawn
 * with, once (topology, blend, stages, combiner, point state). */
/* RECOMP_VKR_PRIMLOG=1: each distinct state a line/point batch is drawn
 * with; =2 also blended triangle batches (trails, effects). With the depth
 * state (test/write/func, W-buffer). */
static uint32_t state_hash(const VkrDrawState *st)
{
    return st->topology * 0x9E3779B1u ^ st->u.stage_program * 31u ^ st->u.color_icw[0] * 7u
        ^ st->u.final0 ^ st->u.final1 * 3u ^ (st->blend_src << 8 | st->blend_dst) * 13u
        ^ (uint32_t)st->blend_enable * 17u ^ st->u.color_ocw[0]
        ^ ((uint32_t)st->depth_test << 1 | (uint32_t)st->depth_write << 2 | (uint32_t)st->w_buffer << 3
           | st->depth_func << 4) * 0x85EBCA6Bu
        ^ ((uint32_t)st->stencil_test | st->stencil_zpass << 1 | st->stencil_func << 17) * 0xC2B2AE35u;
}

static void primlog(const VkrDrawState *st, uint32_t n, int gpu_vsh)
{
    static int on = -1;
    static uint32_t seen[256];
    static int nseen;
    uint32_t h;
    int i;
    if (on < 0) {
        const char *e = getenv("RECOMP_VKR_PRIMLOG");
        on = e ? atoi(e) : 0;
    }
    if (!on || (st->topology == VKR_TOPO_TRIANGLES && (on < 2 || !st->blend_enable)))
        return;
    h = state_hash(st);
    for (i = 0; i < nseen; i++)
        if (seen[i] == h)
            return;
    if (nseen < 256)
        seen[nseen++] = h;
    fprintf(stderr, "[PRIMLOG] %s n %u vsh %d stage_prog %08X tex %d%d%d%d blend %d %X/%X eq %X"
            " rc %u icw0 %08X ocw0 %08X ia0 %08X final %08X %08X fac0 %08X pt %.1f/%d/%d alpha %X/%u"
            " depth test %d write %d func %X wbuf %d stencil %d func %X ref %X mask %X/%X op %X/%X/%X hash %08X\n",
            st->topology == VKR_TOPO_LINES ? "lines" : st->topology == VKR_TOPO_POINTS ? "points" : "tris",
            n, gpu_vsh, st->u.stage_program,
            st->tex[0] != NULL, st->tex[1] != NULL, st->tex[2] != NULL, st->tex[3] != NULL,
            st->blend_enable, st->blend_src, st->blend_dst, st->blend_eq, st->u.control & 0xFF,
            st->u.color_icw[0], st->u.color_ocw[0], st->u.alpha_icw[0], st->u.final0, st->u.final1,
            st->u.factor0[0], st->point_size, st->point_params, st->point_sprite,
            st->alpha_func, st->alpha_ref, st->depth_test, st->depth_write, st->depth_func, st->w_buffer,
            st->stencil_test, st->stencil_func, st->stencil_ref, st->stencil_read_mask, st->stencil_write_mask,
            st->stencil_fail, st->stencil_zfail, st->stencil_zpass, h);
    fflush(stderr);
}

extern volatile int ssx3_autonav_briefed;

/* At a flip: RECOMP_AB. */
static void ab_flip(void)
{
    static int init, pairs = 3, k, phase, wait, from_brief;
    static double start, interval = 4000, t0;
    static char dir[400];
    if (!init) {
        const char *e = getenv("RECOMP_AB"), *c;
        size_t n;
        init = 1;
        t0 = now_ms();                  /* the first flip */
        if (!e)
            return;
        c = strchr(e, ',');
        n = c ? (size_t)(c - e) : strlen(e);
        if (n >= sizeof dir) n = sizeof dir - 1;
        memcpy(dir, e, n);
        /* "b<ms>": after the first briefing autonav passes (the race
         * start: the same camera in every run) */
        if (c && c[1] == 'b') {
            from_brief = 1;
            start = atof(c + 2);
        } else
            start = c ? atof(c + 1) : 45000;
        if (c && (c = strchr(c + 1, ','))) {
            pairs = atoi(c + 1);
            if ((c = strchr(c + 1, ',')))
                interval = atof(c + 1);
        }
    }
    if (!dir[0] || k >= pairs || s_ab_req)
        return;
    if (from_brief) {
        if (!ssx3_autonav_briefed)
            return;
        if (from_brief == 1) {
            from_brief = 2;
            t0 = now_ms();
        }
    }
    if (wait > 0) {
        wait--;
        return;
    }
    /* capture on, features off, capture off, features on: 6 flips apart,
     * more than the frames in flight */
    if (phase == 0 && now_ms() - t0 >= start + k * interval) {
        snprintf(s_ab_path, sizeof s_ab_path, "%s/ab%d_on.ppm", dir, k);
        __atomic_store_n(&s_ab_req, 1, __ATOMIC_RELEASE);
        phase = 1; wait = 6;
    } else if (phase == 1) {
        vkr_ab_off(s_vk, !getenv("RECOMP_AB_KEEP"));     /* KEEP: two frames, both on */
        phase = 2; wait = 6;
    } else if (phase == 2) {
        snprintf(s_ab_path, sizeof s_ab_path, "%s/ab%d_off.ppm", dir, k);
        __atomic_store_n(&s_ab_req, 1, __ATOMIC_RELEASE);
        phase = 3; wait = 6;
    } else if (phase == 3) {
        phase = 0; k++;
        vkr_ab_off(s_vk, 0);
    }
}


/* The scene camera (RECOMP_AO / _HAZE / _SHAFTS / _RIM / _MBLUR): per frame,
 * the map from world space to what the terrain's vertex program outputs --
 * pixels x, y, depth-buffer units z, clip w -- measured by running the
 * program on the CPU at the origin and the three unit points: (x w, y w,
 * z w, w) is linear in the world position, so four runs give the 4x4
 * matrix. Taken from the first draw of a terrain program (RECOMP_SCENE_VSH,
 * default E4F2B8D0, the slopes) each frame; frames without one (menus) get
 * no scene effects. The sun is the program's c[17] (the light direction), the
 * haze colour the draw's fog colour. */
static struct {
    int valid;                  /* this frame */
    double m[4][4];             /* rows: x w, y w, z w, w */
    float sun[3], fog[3];
    double eye[3];
} s_cam;

static int cam_program(uint32_t hash)
{
    static int n = -1;
    static uint32_t want[8];
    int i;
    if (n < 0) {
        const char *l = getenv("RECOMP_SCENE_VSH");
        if (!l) l = "E4F2B8D0";
        n = 0;
        while (*l && n < 8) {
            char *end;
            uint32_t v = (uint32_t)strtoul(l, &end, 16);
            if (end == l) break;
            want[n++] = v;
            l = *end == ',' ? end + 1 : end;
        }
    }
    for (i = 0; i < n; i++)
        if (want[i] == hash)
            return 1;
    return 0;
}

/* Program lists from the environment (hex, comma separated), with a default. */
static int prog_list(const char *env, const char *def, uint32_t *out, int max)
{
    const char *l = getenv(env);
    int n = 0;
    if (!l) l = def;
    while (*l && n < max) {
        char *end;
        uint32_t v = (uint32_t)strtoul(l, &end, 16);
        if (end == l) break;
        out[n++] = v;
        l = *end == ',' ? end + 1 : end;
    }
    return n;
}

/* AO tags: the class of a draw by its vertex program -- 2 ground (the
 * terrain programs, RECOMP_SCENE_VSH), 1 solid world object (RECOMP_AO_SOLID,
 * default F46BE8DC: SSX 3's rocks, trees, rails, signs, gates), 3 rider
 * (RECOMP_AO_RIDER, default E9FDBED0), 0 the rest (trails, particles, sky,
 * HUD). Found with RECOMP_AO_TAGS=prog, which colours every draw by its
 * program. AO: ground receives, solid occludes; motion blur leaves riders
 * sharp. */
static int ao_class(uint32_t hash)
{
    static int ns = -1, nr;
    static uint32_t solid[8], rider[8];
    int i;
    if (cam_program(hash))
        return 2;
    if (ns < 0) {
        ns = prog_list("RECOMP_AO_SOLID", "F46BE8DC", solid, 8);
        nr = prog_list("RECOMP_AO_RIDER", "E9FDBED0", rider, 8);
    }
    for (i = 0; i < ns; i++)
        if (solid[i] == hash)
            return 1;
    for (i = 0; i < nr; i++)
        if (rider[i] == hash)
            return 3;
    return 0;
}

static void scene_camera(const VkrDrawState *st, const float *in, const Nv2aVshProgram *p)
{
    static const double pts[5][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {3, -2, 5}};
    float vin[NV2A_VSH_INPUTS][4];
    double out[5][4];
    int a, k, r;
    if (s_cam.valid || !cam_program(p->hash) || p->in_slot[0] == 0xFF)
        return;
    memset(vin, 0, sizeof vin);
    for (a = 0; a < NV2A_VSH_INPUTS; a++)
        if (p->in_slot[a] != 0xFF)
            memcpy(vin[a], in + p->in_slot[a] * 4, sizeof vin[a]);
    for (k = 0; k < 5; k++) {
        Nv2aVshOutput o;
        /* around the first vertex: the unit offsets stay well conditioned */
        vin[0][0] = (float)(in[p->in_slot[0] * 4 + 0] + pts[k][0]);
        vin[0][1] = (float)(in[p->in_slot[0] * 4 + 1] + pts[k][1]);
        vin[0][2] = (float)(in[p->in_slot[0] * 4 + 2] + pts[k][2]);
        vin[0][3] = 1.0f;
        if (!nv2a_vsh_run((const float (*)[4])vin, &o))
            return;
        out[k][0] = (double)o.pos[0] * o.pos[3]; out[k][1] = (double)o.pos[1] * o.pos[3];
        out[k][2] = (double)o.pos[2] * o.pos[3]; out[k][3] = o.pos[3];
    }
    {
        double base[3] = {in[p->in_slot[0] * 4], in[p->in_slot[0] * 4 + 1], in[p->in_slot[0] * 4 + 2]};
        for (r = 0; r < 4; r++) {
            double c = out[0][r];
            s_cam.m[r][0] = out[1][r] - c; s_cam.m[r][1] = out[2][r] - c; s_cam.m[r][2] = out[3][r] - c;
            /* constant term at the world origin, not at the first vertex */
            s_cam.m[r][3] = c - s_cam.m[r][0] * base[0] - s_cam.m[r][1] * base[1] - s_cam.m[r][2] * base[2];
        }
        {
            /* RECOMP_SCENE_TRACE=1: the fit's error at a fifth point, the
             * eye's distance to the first vertex, once a second */
            static int tr = -1;
            static double last;
            if (tr < 0) tr = getenv("RECOMP_SCENE_TRACE") != NULL;
            if (tr && now_ms() - last > 1000.0) {
                double e = 0, pw[3] = {base[0] + 3, base[1] - 2, base[2] + 5};
                last = now_ms();
                for (r = 0; r < 4; r++) {
                    double v = s_cam.m[r][0] * pw[0] + s_cam.m[r][1] * pw[1] + s_cam.m[r][2] * pw[2] + s_cam.m[r][3];
                    e += fabs(v - out[4][r]) / (fabs(out[4][r]) + 1e-6);
                }
                fprintf(stderr, "[SCENE] fit error %.2e, v0 %.1f %.1f %.1f -> px %.1f %.1f z %.1f w %.3f,"
                        " sun %.3f %.3f %.3f fog %.2f %.2f %.2f\n", e, base[0], base[1], base[2],
                        out[0][0] / out[0][3], out[0][1] / out[0][3], out[0][2] / out[0][3], out[0][3],
                        p->consts[17][0], p->consts[17][1], p->consts[17][2],
                        st->u.fog_color[0], st->u.fog_color[1], st->u.fog_color[2]);
            }
        }
    }
    memcpy(s_cam.sun, p->consts[17], sizeof s_cam.sun);
    memcpy(s_cam.fog, st->u.fog_color, sizeof s_cam.fog);
    {
        /* the eye: where x w = y w = w = 0 (rows 0, 1, 3) */
        double a[3][4], f;
        int i, j, q;
        for (i = 0; i < 3; i++) {
            int row = i < 2 ? i : 3;
            for (j = 0; j < 3; j++) a[i][j] = s_cam.m[row][j];
            a[i][3] = -s_cam.m[row][3];
        }
        for (i = 0; i < 3; i++) {             /* Gauss-Jordan, partial pivot */
            int pv = i;
            for (q = i + 1; q < 3; q++) if (fabs(a[q][i]) > fabs(a[pv][i])) pv = q;
            for (j = 0; j < 4; j++) { double t = a[i][j]; a[i][j] = a[pv][j]; a[pv][j] = t; }
            if (fabs(a[i][i]) < 1e-30) return;
            for (q = 0; q < 3; q++) {
                if (q == i) continue;
                f = a[q][i] / a[i][i];
                for (j = 0; j < 4; j++) a[q][j] -= f * a[i][j];
            }
        }
        for (i = 0; i < 3; i++) s_cam.eye[i] = a[i][3] / a[i][i];
    }
    {
        static int tr = -1;
        static double last;
        if (tr < 0) tr = getenv("RECOMP_SCENE_TRACE") != NULL;
        if (tr && now_ms() - last > 500.0) {
            last = now_ms();
            fprintf(stderr, "[SCENE] eye %.1f %.1f %.1f\n", s_cam.eye[0], s_cam.eye[1], s_cam.eye[2]);
        }
    }
    s_cam.valid = 1;
}

/* 4x4 inverse (Gauss-Jordan, partial pivoting); 0 if singular. */
static int inv4(double a[4][4], double r[4][4])
{
    double m[4][8];
    int i, j, k;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 8; j++)
            m[i][j] = j < 4 ? a[i][j] : (j - 4 == i);
    for (i = 0; i < 4; i++) {
        int p = i;
        for (k = i + 1; k < 4; k++) if (fabs(m[k][i]) > fabs(m[p][i])) p = k;
        if (fabs(m[p][i]) < 1e-300) return 0;
        for (j = 0; j < 8; j++) { double t = m[i][j]; m[i][j] = m[p][j]; m[p][j] = t; }
        for (k = 0; k < 4; k++) {
            double f;
            if (k == i) continue;
            f = m[k][i] / m[i][i];
            for (j = 0; j < 8; j++) m[k][j] -= f * m[i][j];
        }
    }
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
            r[i][j] = m[i][j + 4] / m[i][i];
    return 1;
}

/* M [P, 1] -> guest pixel; 0 if behind the eye. */
static int cam_project(double m[4][4], const double p[3], double *x, double *y)
{
    double v[4];
    int r;
    for (r = 0; r < 4; r++)
        v[r] = m[r][0] * p[0] + m[r][1] * p[1] + m[r][2] * p[2] + m[r][3];
    if (v[3] <= 1e-6)
        return 0;
    *x = v[0] / v[3]; *y = v[1] / v[3];
    return 1;
}

/* At a flip: the frame's camera to the renderer (vkr_scene_set), positions
 * relative to the eye, depth shifted by zmax (scene_common.glsl). */
static int s_scene_sent;            /* vkr_scene_set done this frame */

static void scene_flip(void)
{
    static double prev[4][4];
    static int have_prev;
    double me[4][4], inv[4][4], to_sun[3], len, sx, sy;
    VkrScene sc;
    int r, k;
    if (!s_cam.valid) {
        have_prev = 0;
        return;
    }
    if (s_scene_sent)
        return;
    s_scene_sent = 1;
    memset(&sc, 0, sizeof sc);
    for (r = 0; r < 4; r++) {
        for (k = 0; k < 3; k++) me[r][k] = s_cam.m[r][k];
        me[r][3] = s_cam.m[r][0] * s_cam.eye[0] + s_cam.m[r][1] * s_cam.eye[1]
                 + s_cam.m[r][2] * s_cam.eye[2] + s_cam.m[r][3];
    }
    if (!inv4(me, inv))
        return;
    /* input (gx, gy, d - zmax, 1): column 3 takes zmax times column 2 */
    for (r = 0; r < 4; r++)
        inv[r][3] += inv[r][2] * (double)s_zmax;
    for (r = 0; r < 4; r++)
        for (k = 0; k < 4; k++)
            sc.inv_m[k * 4 + r] = (float)inv[r][k];
    /* last frame's camera on this frame's eye-relative positions */
    for (r = 0; r < 4; r++) {
        double (*pm)[4] = have_prev ? prev : me;
        for (k = 0; k < 3; k++) sc.prev_m[k * 4 + r] = (float)pm[r][k];
        sc.prev_m[12 + r] = (float)(pm[r][0] * s_cam.eye[0] + pm[r][1] * s_cam.eye[1]
                                    + pm[r][2] * s_cam.eye[2] + pm[r][3]);
    }
    memcpy(prev, s_cam.m, sizeof prev);
    have_prev = 1;

    /* c[17] is the light's direction (Y is up and it points down): the sun
     * is the other way */
    len = sqrt((double)s_cam.sun[0] * s_cam.sun[0] + (double)s_cam.sun[1] * s_cam.sun[1]
               + (double)s_cam.sun[2] * s_cam.sun[2]);
    if (len < 1e-6)
        return;
    for (k = 0; k < 3; k++) to_sun[k] = -s_cam.sun[k] / len;
    for (k = 0; k < 3; k++) sc.sun[k] = (float)to_sun[k];
    {
        double far[3];
        for (k = 0; k < 3; k++) far[k] = s_cam.eye[k] + to_sun[k] * 1e7;
        if (cam_project(s_cam.m, far, &sx, &sy)) {
            double ox = sx < 0 ? -sx : sx > s_w ? sx - s_w : 0;
            double oy = sy < 0 ? -sy : sy > s_h ? sy - s_h : 0;
            double out = sqrt(ox * ox + oy * oy) / (0.5 * s_w);
            sc.sun[3] = (float)(out >= 1.0 ? 0.0 : 1.0 - out * out * (3.0 - 2.0 * out));
            sc.sun_px[0] = (float)sx; sc.sun_px[1] = (float)sy;
        }
    }
    {
        /* towards the sun on screen, around a point ahead of the eye */
        double fwd[3], fl, a[3], b[3], ax, ay, bx, by;
        for (k = 0; k < 3; k++) fwd[k] = s_cam.m[3][k];
        fl = sqrt(fwd[0] * fwd[0] + fwd[1] * fwd[1] + fwd[2] * fwd[2]);
        if (fl > 0) {
            for (k = 0; k < 3; k++) {
                a[k] = s_cam.eye[k] + fwd[k] / fl * 500.0;
                b[k] = a[k] + to_sun[k] * 50.0;
            }
            if (cam_project(s_cam.m, a, &ax, &ay) && cam_project(s_cam.m, b, &bx, &by)) {
                double dx = bx - ax, dy = by - ay, dl = sqrt(dx * dx + dy * dy);
                if (dl > 1e-6) { sc.sun_px[2] = (float)(dx / dl); sc.sun_px[3] = (float)(dy / dl); }
            }
        }
    }
    memcpy(sc.fog, s_cam.fog, sizeof s_cam.fog);
    sc.fog[3] = 1.0f;
    vkr_scene_set(s_vk, &sc);
}

/* The first 2D draw (screen space: w == 1) after the world in a frame: the
 * scene pass runs before it, so the HUD stays untouched (vkr_scene_run). */
static int s_scene_ran;

/* RECOMP_SCENE_TRACE=2: every main-target draw of one frame (in game, 3 s
 * after the first camera): index, w, depth state, blend, vertices.
 * s_sdt: 0 waiting, 1 this frame, 2 done. */
static int s_sdt;
static float s_first_z;                 /* z of the draw's first vertex (trace) */

static void scene_trace_draw(float w, const VkrDrawState *st, uint32_t n)
{
    if (s_sdt != 1)
        return;
    fprintf(stderr, "[SCENEDRAW] #%u z %.6g w %.3g depth %d/%d func %X blend %d %X/%X alpha %X n %u stages %05X prog %08X\n",
            s_frame_draw_no, s_first_z, w, st->depth_test, st->depth_write, st->depth_func, st->blend_enable,
            st->blend_src, st->blend_dst, st->alpha_func, n, st->u.stage_program, st->prog);
}

static void scene_before_2d(float w, float z)
{
    /* w == 1 at the far plane is the backdrop (SSX 3's sky sprites, drawn
     * between the terrain and the rocks, trees and riders), not the HUD */
    if (!s_cam.valid || s_scene_ran || w != 1.0f || z >= (float)s_zmax * 0.999f)
        return;
    s_scene_ran = 1;
    {
        static int tr = -1;
        static double last;
        if (tr < 0) tr = getenv("RECOMP_SCENE_TRACE") != NULL;
        if (tr && (s_sdt == 1 || now_ms() - last > 1000.0)) {
            last = now_ms();
            fprintf(stderr, "[SCENE] pass at draw %u of the frame (last frame had %u)\n",
                    s_frame_draw_no, s_frame_draws_last);
        }
    }
    scene_flip();
    vkr_scene_run(s_vk);
}

/* w of the program's first vertex, run on the CPU. */
static float vsh_first_w(const float *in, const Nv2aVshProgram *p)
{
    float vin[NV2A_VSH_INPUTS][4];
    Nv2aVshOutput o;
    int a;
    memset(vin, 0, sizeof vin);
    for (a = 0; a < NV2A_VSH_INPUTS; a++)
        if (p->in_slot[a] != 0xFF)
            memcpy(vin[a], in + p->in_slot[a] * 4, sizeof vin[a]);
    if (!nv2a_vsh_run((const float (*)[4])vin, &o))
        return 0.0f;
    s_first_z = o.pos[2];
    return o.pos[3];
}

static int be_draw(const Nv2aTarget *t, uint32_t prim, const Nv2aVshOutput *v, uint32_t n)
{
    uint32_t *idx, k = 0, topo;

    double td = 0;
    if (s_stats < 0) s_stats = getenv("RECOMP_VKR_STATS") != NULL;
    if (s_stats) { td = now_ms(); stats_tick(); }
    if (!select_target(t)) {
        /* RECOMP_VKR_SKIP_RT=1 (measurement): drop draws into swizzled
         * render targets (the rider shadow maps) instead of handing them to
         * the software rasteriser -- what drawing them costs. */
        static int skip_rt = -1;
        if (skip_rt < 0)
            skip_rt = getenv("RECOMP_VKR_SKIP_RT") != NULL;
        return skip_rt && t->swizzled;
    }
    if (!(idx = build_idx(prim, n, &k, &topo)))
        return 0;
    if (!k)
        return 1;
    /* The vertex program output is relative to the surface origin, which is
     * the target's; the clip rectangle is the scissor (is_main). */
    {
        VkrDrawState st;
        memset(&st, 0, sizeof st);
        fill_state(t, &st);
        st.topology = topo;
        double t2;
        bind_textures(&st);
        primlog(&st, n, 0);
        {
            static int no_shadow = -1;
            if (no_shadow < 0) no_shadow = getenv("RECOMP_VKR_NO_SHADOW") != NULL;
            if (no_shadow && samples_rt(&st))
                return 1;               /* swallowed: experiment */
        }
        if (s_trace == 2)
            trace_draw(t, prim, v, n, &st);
        if (!t->swizzled && !is_rt(t->color_offset) && n) {
            s_first_z = v[0].pos[2];
            scene_trace_draw(v[0].pos[3], &st, n);
            scene_before_2d(v[0].pos[3], v[0].pos[2]);
        }
        t2 = s_stats ? now_ms() : 0;
        vkr_draw(s_vk, (const VkrVertex *)v, n, idx, k, &st);
        if (s_stats) { s_t[3] += now_ms() - t2; s_n[0]++; }
        if (s_trace == 2)
            trace_black(prim, v, n, &st);
    }
    s_dirty = 1;
    if (s_stats) s_t[0] += now_ms() - td;
    return 1;
}

/* The draw with the vertex program left to the GPU. Declined (so the CPU
 * runs it and calls be_draw) for other surfaces, and while a trace that needs
 * the transformed vertices is on. */
static int be_draw_vsh(const Nv2aTarget *t, uint32_t prim, const float *in, uint32_t n,
                       const Nv2aVshProgram *p)
{
    uint32_t *idx, k = 0, topo;
    double td = 0;
    if (s_trace == 2)
        return 0;
    if (s_stats < 0) s_stats = getenv("RECOMP_VKR_STATS") != NULL;
    if (s_stats) { td = now_ms(); stats_tick(); }
    if (!select_target(t))
        return 0;
    if (!(idx = build_idx(prim, n, &k, &topo)))
        return 0;
    if (!k)
        return 1;
    {
        VkrDrawState st;
        double t2;
        memset(&st, 0, sizeof st);
        fill_state(t, &st);
        st.topology = topo;
        bind_textures(&st);
        primlog(&st, n, 1);
        scene_camera(&st, in, p);
        {
            static int no_shadow = -1;
            if (no_shadow < 0) no_shadow = getenv("RECOMP_VKR_NO_SHADOW") != NULL;
            if (no_shadow && samples_rt(&st))
                return 1;
        }
        {
            /* RECOMP_DUMP_CONSTS=1: each distinct program's constants, once
             * (finding the camera projection for the widescreen work). */
            static int on = -1, nseen;
            static uint32_t seen[64];
            int i;
            if (on < 0) on = getenv("RECOMP_DUMP_CONSTS") != NULL;
            if (on) {
                for (i = 0; i < nseen && seen[i] != p->hash; i++)
                    ;
                if (i == nseen && nseen < 64) {
                    seen[nseen++] = p->hash;
                    fprintf(stderr, "[CONSTS] program %08X (%u insns) prim %u n %u\n", p->hash, p->nins, prim, n);
                    for (i = 0; i < 192; i++)
                        if (p->consts[i][0] || p->consts[i][1] || p->consts[i][2] || p->consts[i][3])
                            fprintf(stderr, "[CONSTS]   c[%d] %g %g %g %g\n", i, p->consts[i][0],
                                    p->consts[i][1], p->consts[i][2], p->consts[i][3]);
                }
            }
        }
        st.prog = p->hash;
        st.ao_class = ao_class(p->hash);
        if (s_cam.valid && !t->swizzled && !is_rt(t->color_offset) && n) {
            float w0 = vsh_first_w(in, p);
            scene_trace_draw(w0, &st, n);
            if (!s_scene_ran)
                scene_before_2d(w0, s_first_z);
        }
        t2 = s_stats ? now_ms() : 0;
        if (!vkr_draw_vsh(s_vk, in, n, p->nin, &p->ins[0][0], p->nins, p->hash, p->consts,
                          idx, k, &st))
            return 0;
        if (s_stats) { s_t[3] += now_ms() - t2; s_n[0]++; }
    }
    s_dirty = 1;
    if (s_stats) s_t[0] += now_ms() - td;
    return 1;
}

/* RECOMP_VKR_CLEARLOG=1: once a second, each distinct clear of the main
 * target (flags, clip rectangle, depth value) with how often it ran. */
static void clearlog(const Nv2aTarget *t, uint32_t flags, float depth)
{
    static int on = -1;
    static struct { uint32_t flags, x, y, w, h; float d; unsigned n; } e[16];
    static int ne;
    static double t0;
    int i;
    if (on < 0) on = getenv("RECOMP_VKR_CLEARLOG") != NULL;
    if (!on)
        return;
    for (i = 0; i < ne; i++)
        if (e[i].flags == flags && e[i].x == t->x && e[i].y == t->y && e[i].w == t->width
            && e[i].h == t->height && e[i].d == depth)
            break;
    if (i == ne && ne < 16) {
        e[i].flags = flags; e[i].x = t->x; e[i].y = t->y; e[i].w = t->width; e[i].h = t->height;
        e[i].d = depth; e[i].n = 0; ne++;
    }
    if (i < ne)
        e[i].n++;
    if (now_ms() - t0 >= 1000.0) {
        for (i = 0; i < ne; i++)
            fprintf(stderr, "[CLEARLOG] flags %X clip %u,%u %ux%u depth %g x%u\n", e[i].flags, e[i].x,
                    e[i].y, e[i].w, e[i].h, e[i].d, e[i].n);
        ne = 0;
        t0 = now_ms();
    }
}

static int be_clear(const Nv2aTarget *t, uint32_t flags, uint32_t argb, float depth)
{
    float c[4];
    if (!select_target(t))
        return 0;
    clearlog(t, flags, depth);
    c[0] = (float)((argb >> 16) & 0xFF) / 255.0f;
    c[1] = (float)((argb >> 8) & 0xFF) / 255.0f;
    c[2] = (float)(argb & 0xFF) / 255.0f;
    c[3] = (float)(argb >> 24) / 255.0f;
    if (!(flags & (NV2A_CLEAR_COLOR | NV2A_CLEAR_Z | NV2A_CLEAR_STENCIL)))
        return 1;
    if (flags & NV2A_CLEAR_COLOR)
        vkr_clear(s_vk, c, 0, 0.0f);
    /* depth / stencil: colour untouched */
    vkr_clear_ds(s_vk, (flags & NV2A_CLEAR_Z) != 0, depth,
                 (flags & NV2A_CLEAR_STENCIL) != 0, t->stencil_clear);
    s_dirty = 1;
    return 1;
}

/* RECOMP_WIDESCREEN=auto: the frame takes the window's shape. Checked at
 * flips (10/s); a new shape is applied once the window has kept it for
 * 250 ms (not on every step of a drag), by rebuilding the target between
 * frames, and the camera (ssx3_Aspect) follows from the next frame on.
 * Narrower than 4:3 stays 4:3; wider than 8:1 stays 8:1. */
static void follow_window(void)
{
    static int on = -1;
    static double t_check, t_seen;
    static float seen;
    double now;
    float a, cur;
    int cw, ch;
    if (on < 0) {
        const char *e = getenv("RECOMP_WIDESCREEN");
        on = e && !strcmp(e, "auto");
    }
    if (!on || !s_w)
        return;
    now = now_ms();
    if (now - t_check < 100.0)
        return;
    t_check = now;
    if (!xbox_FramebufferClientSize(&cw, &ch))
        return;
    a = (float)cw / (float)ch;
    if (a < 4.0f / 3.0f) a = 4.0f / 3.0f;
    if (a > 8.0f) a = 8.0f;
    cur = ssx3_Aspect();
    if (fabsf(a - cur) < cur * 0.01f) {
        seen = 0.0f;
        return;
    }
    if (fabsf(a - seen) >= seen * 0.01f) {
        seen = a;
        t_seen = now;
        return;
    }
    if (now - t_seen < 250.0)
        return;
    ssx3_SetAspect(a);
    vkr_set_aspect(s_vk, ssx3_Aspect());
    seen = 0.0f;
    if (!vkr_set_target(s_vk, s_w, s_h, (float)s_zmax)) {
        s_w = s_h = 0;      /* rebuilt at the next draw */
        return;
    }
    fprintf(stderr, "[VKR] window %dx%d: frame aspect %.3f\n", cw, ch, ssx3_Aspect());
}


static void be_flip(const Nv2aTarget *t)
{
    FlipJob j;
    double tf;
    ssx3_last_flip_ms = xbox_TickCountMs();
    ab_flip();
#ifdef _WIN32
    ssx3_exec_tid = (uint32_t)GetCurrentThreadId();
#endif
    if (s_trace < 0) {
        const char *e = getenv("RECOMP_VKR_TRACE_AT");
        s_trace = e ? 1 : 0;
        s_trace_at = e ? atof(e) : 0;
        e = getenv("RECOMP_VKR_TRACE_EVERY");
        s_trace_every = e ? atof(e) : 0;
        s_t0 = now_ms();
        /* RECOMP_VKR_TRACE_BRIEF=<ms>: the first traced frame that long
         * after autonav reached the briefing (what the probe scripts time
         * their shots from) instead of after start. */
        e = getenv("RECOMP_VKR_TRACE_BRIEF");
        if (e) {
            s_trace = 1;
            s_trace_brief = atof(e);
            s_trace_at = 1e18;
        }
    }
    if (s_trace_brief >= 0 && g_ssx3_at_briefing) {
        s_trace_at = now_ms() - s_t0 + s_trace_brief;
        s_trace_brief = -1;
    }
    if (s_trace == 2) {
        s_trace = s_trace_every > 0 ? 1 : 0;
        s_trace_at += s_trace_every;
        fprintf(stderr, "[VKRTRACE] end of frame %u\n", s_trace_no);
        if (s_tbf)
            fflush(s_tbf);
    } else if (s_trace == 1 && now_ms() - s_t0 >= s_trace_at) {
        s_trace = 2;
        s_draw_no = 0;
        s_trace_no++;
        fprintf(stderr, "[VKRTRACE] frame %u at %.0f ms (%u)\n", s_trace_no, now_ms() - s_t0,
                (unsigned)xbox_TickCountMs());
    }
    {
        int i;
        for (i = 0; i < s_nfront && s_front[i] != t->color_offset; i++)
            ;
        if (i == s_nfront && s_nfront < 4) {
            s_front[s_nfront++] = t->color_offset;
            fprintf(stderr, "[VKR] front buffer %08X\n", t->color_offset);
        }
    }
    if (!s_dirty || t->bpp != 4 || t->pitch / 4 < s_w)
        return;
    tf = s_stats ? now_ms() : 0;
    scene_flip();
    j.slot = vkr_flip_submit(s_vk);
    if (s_stats) { s_t[4] += now_ms() - tf; s_n[2]++; }
    if (j.slot < 0)
        return;
    /* the target is the surface from its origin (is_main) */
    j.offset = t->color_offset; j.pitch = t->pitch;
    j.x = 0; j.y = 0; j.w = s_w; j.h = s_h;
    pthread_mutex_lock(&s_job_lock);
    s_jobs[(s_job_head + s_job_n) % 4] = j;  /* <= VKR_FRAMES pending: never full */
    s_job_n++;
    pthread_cond_signal(&s_job_cond);
    pthread_mutex_unlock(&s_job_lock);
    s_dirty = 0;
    {
        int had_cam = s_cam.valid;
        static int tr = -1;
        static double t0;
        if (tr < 0) { const char *e = getenv("RECOMP_SCENE_TRACE"); tr = e ? atoi(e) : 0; }
        if (s_sdt == 1)
            s_sdt = 2;
        else if (tr == 2 && s_sdt == 0 && had_cam) {
            /* RECOMP_SCENE_TRACE_AT=<ms>: that long after the first race
             * briefing instead (the same frame as RECOMP_AB=..,b<ms>) */
            const char *at = getenv("RECOMP_SCENE_TRACE_AT");
            if (at) {
                if (ssx3_autonav_briefed && !t0) t0 = now_ms();
                if (t0 && now_ms() - t0 > atof(at)) s_sdt = 1;
            } else if (!t0) t0 = now_ms();
            else if (now_ms() - t0 > 3000.0) s_sdt = 1;
        }
    }
    s_cam.valid = 0;
    s_scene_sent = s_scene_ran = 0;
    s_frame_draws_last = s_frame_draw_no;
    s_frame_draw_no = 0;
    follow_window();
}

static const Nv2aBackend s_backend = {be_draw, be_clear, be_flip, be_draw_vsh};

int vkr_backend_install(void)
{
    pthread_t th;
    s_vk = vkr_create();
    if (!s_vk)
        return 0;
    if (pthread_create(&th, NULL, flip_thread, NULL) != 0)
        return 0;
    pthread_detach(th);
    nv2a_exec_set_backend(&s_backend);
    fprintf(stderr, "[VKR] backend installed\n");
    return 1;
}
