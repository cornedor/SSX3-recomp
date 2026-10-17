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
extern void xbox_FramebufferPublishImage(const uint32_t *px, uint32_t w, uint32_t h);
extern int ssx3_WidescreenNow(void);
extern float ssx3_Aspect(void);
extern void ssx3_SetAspect(float a);
extern int xbox_FramebufferClientSize(int *w, int *h);
extern void ssx3_FpsOverlay(uint32_t *px, int w, int h, int stride);
extern void ssx3_GearsOverlay(uint32_t *px, int w, int h, int stride);

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
            /* First-person shooter mode: crosshair and gun (src/fps_mode.c);
             * the gearbox HUD (src/gears_mode.c). */
            ssx3_FpsOverlay((uint32_t *)(mem + j.offset + (size_t)j.y * j.pitch + (size_t)j.x * 4),
                            (int)j.w, (int)j.h, (int)(j.pitch / 4));
            ssx3_GearsOverlay((uint32_t *)(mem + j.offset + (size_t)j.y * j.pitch + (size_t)j.x * 4),
                              (int)j.w, (int)j.h, (int)(j.pitch / 4));
            /* Higher internal resolution: the window shows the full image. */
            full = vkr_frame_pixels_full(s_vk, j.slot, &fw, &fh);
            if (full) {
                ssx3_FpsOverlay((uint32_t *)full, (int)fw, (int)fh, (int)fw);
                ssx3_GearsOverlay((uint32_t *)full, (int)fw, (int)fh, (int)fw);
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


static int is_rt(uint32_t off)
{
    int i;
    for (i = 0; i < s_nrt; i++)
        if (s_rt[i] == off)
            return 1;
    return 0;
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
        tex = vkr_tex_find(s_vk, ti.hash);
        if (s_stats) s_t[1] += now_ms() - t0;
        if (!tex) {
            double t1 = s_stats ? now_ms() : 0;
            uint32_t rows = ti.cube ? ti.height * 6 : ti.height;  /* cube: faces stacked */
            uint32_t *px = (uint32_t *)malloc((size_t)ti.width * rows * 4);
            int rt = is_rt(ti.offset), j;
            if (px && nv2a_exec_tex_decode(k, px)) {
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
                    tex = vkr_tex_create(s_vk, ti.hash, ti.width, rows, px);
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
static unsigned s_trace_no;

static void trace_draw(const Nv2aTarget *t, uint32_t prim, const Nv2aVshOutput *v, uint32_t n,
                       const VkrDrawState *st)
{
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    uint32_t i;
    int k;
    for (i = 0; i < n; i++) {
        if (v[i].pos[0] < x0) x0 = v[i].pos[0];
        if (v[i].pos[0] > x1) x1 = v[i].pos[0];
        if (v[i].pos[1] < y0) y0 = v[i].pos[1];
        if (v[i].pos[1] > y1) y1 = v[i].pos[1];
    }
    if ((x1 - x0) * (y1 - y0) < 2000.0f)
        return;
    fprintf(stderr, "[VKRTRACE] prim %u n %u box %.0f,%.0f-%.0f,%.0f w0 %.3g d0 %.2f,%.2f,%.2f,%.2f | stages %05X",
            prim, n, x0, y0, x1, y1, v[0].pos[3], v[0].d0[0], v[0].d0[1], v[0].d0[2], v[0].d0[3],
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
static void primlog(const VkrDrawState *st, uint32_t n, int gpu_vsh)
{
    static int on = -1;
    static uint32_t seen[64];
    static int nseen;
    uint32_t h;
    int i;
    if (on < 0) on = getenv("RECOMP_VKR_PRIMLOG") != NULL;
    if (!on || st->topology == VKR_TOPO_TRIANGLES)
        return;
    h = st->topology * 0x9E3779B1u ^ st->u.stage_program * 31u ^ st->u.color_icw[0] * 7u
        ^ st->u.final0 ^ st->u.final1 * 3u ^ (st->blend_src << 8 | st->blend_dst) * 13u
        ^ (uint32_t)st->blend_enable * 17u ^ st->u.color_ocw[0];
    for (i = 0; i < nseen; i++)
        if (seen[i] == h)
            return;
    if (nseen < 64)
        seen[nseen++] = h;
    fprintf(stderr, "[PRIMLOG] %s n %u vsh %d stage_prog %08X tex %d%d%d%d blend %d %X/%X eq %X"
            " rc %u icw0 %08X ocw0 %08X ia0 %08X final %08X %08X fac0 %08X pt %.1f/%d/%d alpha %X/%u\n",
            st->topology == VKR_TOPO_LINES ? "lines" : "points", n, gpu_vsh, st->u.stage_program,
            st->tex[0] != NULL, st->tex[1] != NULL, st->tex[2] != NULL, st->tex[3] != NULL,
            st->blend_enable, st->blend_src, st->blend_dst, st->blend_eq, st->u.control & 0xFF,
            st->u.color_icw[0], st->u.color_ocw[0], st->u.alpha_icw[0], st->u.final0, st->u.final1,
            st->u.factor0[0], st->point_size, st->point_params, st->point_sprite,
            st->alpha_func, st->alpha_ref);
    fflush(stderr);
}

static int be_draw(const Nv2aTarget *t, uint32_t prim, const Nv2aVshOutput *v, uint32_t n)
{
    uint32_t *idx, k = 0, topo;

    double td = 0;
    if (s_stats < 0) s_stats = getenv("RECOMP_VKR_STATS") != NULL;
    if (s_stats) { td = now_ms(); stats_tick(); }
    if (!is_main(t)) {
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
    if (!is_main(t))
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

static int be_clear(const Nv2aTarget *t, uint32_t flags, uint32_t argb, float depth)
{
    float c[4];
    if (!is_main(t))
        return 0;
    c[0] = (float)((argb >> 16) & 0xFF) / 255.0f;
    c[1] = (float)((argb >> 8) & 0xFF) / 255.0f;
    c[2] = (float)(argb & 0xFF) / 255.0f;
    c[3] = (float)(argb >> 24) / 255.0f;
    if (!(flags & NV2A_CLEAR_COLOR) && !(flags & NV2A_CLEAR_Z))
        return 1;
    if (flags & NV2A_CLEAR_COLOR)
        vkr_clear(s_vk, c, (flags & NV2A_CLEAR_Z) != 0, depth);
    else {
        /* depth-only clear: colour untouched */
        vkr_clear_depth(s_vk, depth);
    }
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
        fprintf(stderr, "[VKRTRACE] frame %u at %.0f ms\n", s_trace_no, now_ms() - s_t0);
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
