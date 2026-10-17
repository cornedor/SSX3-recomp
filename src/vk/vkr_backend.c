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
typedef struct { int slot; uint32_t offset, pitch, x, y, w, h; int shot; } FlipJob;
static void mbtest_write(const FlipJob *j, const uint8_t *px, uint32_t w, uint32_t h);
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
            if (full)
                xbox_FramebufferPublishImage((const uint32_t *)full, fw, fh);
            if (full && j.shot)
                mbtest_write(&j, full, fw, fh);
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
static double s_trace_brief = -1;
extern volatile int g_ssx3_at_briefing;     /* autonav.c */
extern int xbox_PadInject(const char *line);   /* usb_gamepad.c */
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

/* The scene camera (RECOMP_MBLUR): per frame, the map from world space to
 * what the terrain's vertex program outputs -- pixels x, y, depth-buffer
 * units z, clip w -- measured by running the program on the CPU at the
 * origin and the three unit points: (x w, y w, z w, w) is linear in the world
 * position, so four runs give the 4x4 matrix. Taken from the first draw of a
 * terrain program (RECOMP_SCENE_VSH, default E4F2B8D0, the slopes) each
 * frame; frames without one (menus) get no motion blur. */
static struct {
    int valid;                  /* this frame */
    double m[4][4];             /* rows: x w, y w, z w, w */
    double eye[3];
} s_cam;

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

static int cam_program(uint32_t hash)
{
    static int n = -1;
    static uint32_t want[8];
    int i;
    if (n < 0)
        n = prog_list("RECOMP_SCENE_VSH", "E4F2B8D0", want, 8);
    for (i = 0; i < n; i++)
        if (want[i] == hash)
            return 1;
    return 0;
}

/* Rider tags: a rider's draw by its vertex program (RECOMP_MBLUR_RIDER,
 * default E9FDBED0 SSX 3's riders, 5D983485 their boards -- two draws a
 * frame at the rider's distance, RECOMP_MBLUR_PROGS); the motion blur
 * leaves them sharp. */
static int rider_program(uint32_t hash)
{
    static int n = -1;
    static uint32_t rider[8];
    int i;
    if (n < 0)
        n = prog_list("RECOMP_MBLUR_RIDER", "E9FDBED0,5D983485", rider, 8);
    for (i = 0; i < n; i++)
        if (rider[i] == hash)
            return 1;
    return 0;
}

static void scene_camera(const float *in, const Nv2aVshProgram *p)
{
    /* the base vertex, then +-h along each axis: the program's output times
     * w is linear in the input, so a large step is exact and keeps its float
     * rounding (world coordinates ~1e5) out of the differences -- with a
     * unit step the fit changed with whichever vertex came first, and a
     * still camera showed motion. h stays under 10% of the vertex's w, so
     * no sample reaches the eye plane. */
    static const double pts[7][3] = {{0, 0, 0}, {1, 0, 0}, {-1, 0, 0}, {0, 1, 0},
                                     {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    float vin[NV2A_VSH_INPUTS][4];
    double out[7][4], h = 1.0;
    int a, k, r;
    if (s_cam.valid || !cam_program(p->hash) || p->in_slot[0] == 0xFF)
        return;
    memset(vin, 0, sizeof vin);
    for (a = 0; a < NV2A_VSH_INPUTS; a++)
        if (p->in_slot[a] != 0xFF)
            memcpy(vin[a], in + p->in_slot[a] * 4, sizeof vin[a]);
    for (k = 0; k < 7; k++) {
        Nv2aVshOutput o;
        vin[0][0] = (float)(in[p->in_slot[0] * 4 + 0] + pts[k][0] * h);
        vin[0][1] = (float)(in[p->in_slot[0] * 4 + 1] + pts[k][1] * h);
        vin[0][2] = (float)(in[p->in_slot[0] * 4 + 2] + pts[k][2] * h);
        vin[0][3] = 1.0f;
        if (!nv2a_vsh_run((const float (*)[4])vin, &o))
            return;
        out[k][0] = (double)o.pos[0] * o.pos[3]; out[k][1] = (double)o.pos[1] * o.pos[3];
        out[k][2] = (double)o.pos[2] * o.pos[3]; out[k][3] = o.pos[3];
        if (k == 0) {
            if (!(o.pos[3] > 1.0f))
                return;
            h = fmin(256.0, fmax(1.0, 0.1 * o.pos[3]));
        } else if (!(o.pos[3] > 0.0f))
            return;
    }
    {
        double base[3] = {in[p->in_slot[0] * 4], in[p->in_slot[0] * 4 + 1], in[p->in_slot[0] * 4 + 2]};
        for (r = 0; r < 4; r++) {
            for (k = 0; k < 3; k++)
                s_cam.m[r][k] = (out[1 + 2 * k][r] - out[2 + 2 * k][r]) / (2.0 * h);
            /* constant term at the world origin, not at the first vertex */
            s_cam.m[r][3] = out[0][r] - s_cam.m[r][0] * base[0] - s_cam.m[r][1] * base[1] - s_cam.m[r][2] * base[2];
        }
    }
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

/* At a flip: the frame's camera to the renderer (vkr_scene_set), positions
 * relative to the eye, depth shifted by zmax (scene_common.glsl). */
static int s_scene_sent;            /* vkr_scene_set done this frame */

/* Frame-to-frame camera numbers for the test harness (RECOMP_MBLUR_TEST)
 * and RECOMP_MBLUR_TRACE: speed in world units per frame, the largest
 * reprojection on a 5x5 grid of points 30 m away in guest pixels. */
static float s_mb_speed, s_mb_px;
static int s_mb_off_next;           /* the next frame without the pass (reference shot) */
static uint32_t s_mb_at_draw, s_draw_no, s_draw_total;  /* trace: the pass ran before draw N of the frame's total */

/* (gx, gy) in guest pixels of an eye-relative point through camera m
 * (eye-relative, 4x4); 0 when it is behind the camera */
static int cam_project(double m[4][4], const double p[3], double *gx, double *gy)
{
    double c[4];
    int r;
    for (r = 0; r < 4; r++)
        c[r] = m[r][0] * p[0] + m[r][1] * p[1] + m[r][2] * p[2] + m[r][3];
    if (c[3] <= 1e-6)
        return 0;
    *gx = c[0] / c[3]; *gy = c[1] / c[3];
    return 1;
}

static void scene_flip(void)
{
    static double prev[4][4], prev_eye[3], speed;
    static int have_prev;
    double me[4][4], pe[4][4], inv[4][4], step;
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
    step = have_prev ? sqrt((s_cam.eye[0] - prev_eye[0]) * (s_cam.eye[0] - prev_eye[0])
                            + (s_cam.eye[1] - prev_eye[1]) * (s_cam.eye[1] - prev_eye[1])
                            + (s_cam.eye[2] - prev_eye[2]) * (s_cam.eye[2] - prev_eye[2])) : 0.0;
    /* a camera cut (a jump of over 5 m, or a different view): no blur this
     * frame -- it would smear the whole picture */
    if (step > 500.0)
        have_prev = 0;
    /* input (gx, gy, d - zmax, 1): column 3 takes zmax times column 2 */
    for (r = 0; r < 4; r++)
        for (k = 0; k < 4; k++)
            sc.inv_m[k * 4 + r] = (float)(inv[r][k] + (k == 3 ? inv[r][2] * (double)s_zmax : 0.0));
    /* last frame's camera on this frame's eye-relative positions */
    for (r = 0; r < 4; r++) {
        double (*pm)[4] = have_prev ? prev : me;
        for (k = 0; k < 3; k++) pe[r][k] = pm[r][k];
        pe[r][3] = pm[r][0] * s_cam.eye[0] + pm[r][1] * s_cam.eye[1] + pm[r][2] * s_cam.eye[2] + pm[r][3];
        for (k = 0; k < 4; k++)
            sc.prev_m[k * 4 + r] = (float)pe[r][k];
    }
    /* speed (smoothed over ~5 frames: a steady streak, not a pulsing one)
     * and the point the camera heads for */
    speed = have_prev ? speed + (step - speed) * 0.2 : 0.0;
    if (have_prev && step > 1.0) {
        double dir[3], gx, gy;
        for (k = 0; k < 3; k++)
            dir[k] = (s_cam.eye[k] - prev_eye[k]) / step * 1e5;
        if (cam_project(me, dir, &gx, &gy)) {
            sc.foe[0] = (float)gx; sc.foe[1] = (float)gy;
            sc.speed = (float)speed;
        }
    }
    {
        /* the reprojection on a grid at 30 m: zero for a still camera */
        double worst = 0.0;
        int gxn, gyn;
        for (gyn = 0; gyn < 5 && have_prev; gyn++)
            for (gxn = 0; gxn < 5; gxn++) {
                double q[4] = {(gxn + 0.5) / 5.0 * s_w, (gyn + 0.5) / 5.0 * s_h, 0.0, 1.0}, P[4], n, ax, ay, bx, by;
                q[2] = 0.5 * (double)s_zmax;
                for (r = 0; r < 4; r++)
                    P[r] = inv[r][0] * q[0] + inv[r][1] * q[1] + inv[r][2] * q[2] + inv[r][3];
                if (fabs(P[3]) < 1e-12) continue;
                for (k = 0; k < 3; k++) P[k] /= P[3];
                n = sqrt(P[0] * P[0] + P[1] * P[1] + P[2] * P[2]);
                if (n < 1e-9) continue;
                for (k = 0; k < 3; k++) P[k] *= 3000.0 / n;
                if (cam_project(me, P, &ax, &ay) && cam_project(pe, P, &bx, &by))
                    worst = fmax(worst, sqrt((ax - bx) * (ax - bx) + (ay - by) * (ay - by)));
            }
        s_mb_speed = (float)speed;
        s_mb_px = (float)worst;
    }
    {
        /* RECOMP_MBLUR_TRACE=1: once a second, speed and reprojection */
        static int tr = -1;
        static double last, wpx;
        if (tr < 0) tr = getenv("RECOMP_MBLUR_TRACE") != NULL;
        wpx = fmax(wpx, s_mb_px);
        if (tr && now_ms() - last > 1000.0) {
            fprintf(stderr, "[MBLUR] speed %.1f/frame (%.0f km/h at 60 Hz), reprojection max %.2f px,"
                    " pass at draw %u of %u\n", speed, speed * 60.0 * 0.036, wpx, s_mb_at_draw, s_draw_total);
            last = now_ms();
            wpx = 0.0;
        }
    }
    sc.off = s_mb_off_next;
    s_mb_off_next = 0;
    memcpy(prev, s_cam.m, sizeof prev);
    memcpy(prev_eye, s_cam.eye, sizeof prev_eye);
    have_prev = 1;
    vkr_scene_set(s_vk, &sc);
}

/* The first 2D draw (screen space: w == 1) after the world in a frame: the
 * scene pass runs before it, so the HUD stays untouched (vkr_scene_run). */
static int s_scene_ran;

static void scene_before_2d(float w, float z)
{
    /* w == 1 at the far plane is the backdrop (SSX 3's sky sprites, drawn
     * between the terrain and the rocks, trees and riders), not the HUD */
    if (!s_cam.valid || s_scene_ran || w != 1.0f || z >= (float)s_zmax * 0.999f)
        return;
    s_scene_ran = 1;
    s_mb_at_draw = s_draw_no;
    scene_flip();
    vkr_scene_run(s_vk);
}

/* w (and z) of the program's first vertex, run on the CPU. */
static float vsh_first_w(const float *in, const Nv2aVshProgram *p, float *z)
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
    *z = o.pos[2];
    return o.pos[3];
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
        if (n)
            scene_before_2d(v[0].pos[3], v[0].pos[2]);
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

/* RECOMP_MBLUR_PROGS=1: finding programs to keep sharp (the board). Each
 * frame's main-target draws are kept as (program, distance of the first
 * vertex); at the flip, programs drawn at the rider's distance (0.7..1.4 x
 * the nearest rider draw's) are counted, and once a second the counts are
 * logged: [MBLUR-PROGS] hash:draws ... */
static int s_progs = -1, s_nprog_draw;
static struct { uint32_t hash; float w; } s_prog_draw[1024];

static void progs_note(uint32_t hash, float w)
{
    if (s_progs < 0) s_progs = getenv("RECOMP_MBLUR_PROGS") != NULL;
    if (s_progs && s_nprog_draw < 1024 && w > 0.0f) {
        s_prog_draw[s_nprog_draw].hash = hash;
        s_prog_draw[s_nprog_draw++].w = w;
    }
}

static void progs_flip(void)
{
    static uint32_t hs[32], cnt[32];
    static int nh;
    static double last;
    float rw = 0.0f;
    int i, j;
    if (s_progs <= 0)
        return;
    for (i = 0; i < s_nprog_draw; i++)
        if (rider_program(s_prog_draw[i].hash) && (rw == 0.0f || s_prog_draw[i].w < rw))
            rw = s_prog_draw[i].w;
    for (i = 0; rw > 0.0f && i < s_nprog_draw; i++) {
        float r = s_prog_draw[i].w / rw;
        if (r < 0.7f || r > 1.4f || rider_program(s_prog_draw[i].hash))
            continue;
        for (j = 0; j < nh && hs[j] != s_prog_draw[i].hash; j++)
            ;
        if (j == nh && nh < 32) hs[nh++] = s_prog_draw[i].hash;
        if (j < nh) cnt[j]++;
    }
    s_nprog_draw = 0;
    if (now_ms() - last > 1000.0) {
        char buf[600];
        int n = 0;
        last = now_ms();
        for (j = 0; j < nh && n < (int)sizeof buf - 24; j++)
            n += snprintf(buf + n, sizeof buf - n, " %08X:%u", hs[j], cnt[j]);
        fprintf(stderr, "[MBLUR-PROGS] rider w %.0f |%s\n", rw, nh ? buf : " none");
        nh = 0;
        memset(cnt, 0, sizeof cnt);
    }
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
        scene_camera(in, p);
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
        st.rider = rider_program(p->hash);
        s_draw_no++;
        if (s_cam.valid && !s_scene_ran && n) {
            float z0 = 0.0f, w0 = vsh_first_w(in, p, &z0);
            scene_before_2d(w0, z0);
        }
        if (s_progs != 0 && n) {
            float z0 = 0.0f;
            progs_note(p->hash, vsh_first_w(in, p, &z0));
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
    if (!is_main(t))
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


/* RECOMP_MBLUR_TEST=<dir>: a repeatable motion blur test, with
 * RECOMP_AUTONAV=race. Timed from the briefing (autonav presses A there):
 * the stick is held back from the start for brake= ms so the other riders
 * pass in front, then released; at each shot time a pair of frames is
 * saved -- <dir>/NN_blur.ppm and, the frame after, NN_ref.ppm without the
 * pass -- with the camera's speed and reprojection ([MBLUR-TEST]); after
 * the last one the process exits. RECOMP_MBLUR_TEST_PLAN=brake=<ms>,
 * shots=<ms>:<ms>:... (default brake=9000, shots at 4, 7, 10, 13, 16, 19,
 * 22, 25 s). */
static const char *s_mbt_dir;
static int s_mbt = -1, s_mbt_n, s_mbt_next, s_mbt_pending;
static uint32_t s_mbt_shots[32], s_mbt_brake = 9000;
static double s_mbt_t0;

static void mbtest_write(const FlipJob *j, const uint8_t *px, uint32_t w, uint32_t h)
{
    char path[1024];
    FILE *f;
    uint32_t i, n = w * h;
    uint8_t *row;
    snprintf(path, sizeof path, "%s/%02d_%s.ppm", s_mbt_dir, (j->shot - 1) / 2, j->shot & 1 ? "blur" : "ref");
    if (!(f = fopen(path, "wb")) || !(row = (uint8_t *)malloc((size_t)n * 3))) {
        if (f) fclose(f);
        return;
    }
    for (i = 0; i < n; i++) {           /* B G R A -> R G B */
        row[i * 3 + 0] = px[i * 4 + 2];
        row[i * 3 + 1] = px[i * 4 + 1];
        row[i * 3 + 2] = px[i * 4 + 0];
    }
    fprintf(f, "P6 %u %u 255\n", w, h);
    fwrite(row, 3, n, f);
    fclose(f);
    free(row);
    if ((j->shot - 1) / 2 + 1 >= s_mbt_n && !(j->shot & 1)) {
        fprintf(stderr, "[MBLUR-TEST] done\n");
        fflush(NULL);
        _exit(0);
    }
}

/* At each flip (recording side): the job's shot number, 0 = none. */
static int mbtest_flip(void)
{
    double t;
    if (s_mbt < 0) {
        const char *e = getenv("RECOMP_MBLUR_TEST"), *plan = getenv("RECOMP_MBLUR_TEST_PLAN");
        static const uint32_t def[] = {4000, 7000, 10000, 13000, 16000, 19000, 22000, 25000};
        s_mbt = e && *e;
        s_mbt_dir = e;
        memcpy(s_mbt_shots, def, sizeof def);
        s_mbt_n = 8;
        if (plan) {
            const char *b = strstr(plan, "brake="), *sh = strstr(plan, "shots=");
            if (b) s_mbt_brake = (uint32_t)strtoul(b + 6, NULL, 10);
            if (sh) {
                char *end;
                sh += 6;
                for (s_mbt_n = 0; s_mbt_n < 32; ) {
                    s_mbt_shots[s_mbt_n++] = (uint32_t)strtoul(sh, &end, 10);
                    if (*end != ':') break;
                    sh = end + 1;
                }
            }
        }
    }
    if (!s_mbt)
        return 0;
    if (!g_ssx3_at_briefing)
        return 0;
    t = now_ms();
    if (s_mbt_t0 == 0.0) {
        char line[32];
        s_mbt_t0 = t;
        if (s_mbt_brake) {
            snprintf(line, sizeof line, "ldown:%u", s_mbt_brake);
            xbox_PadInject(line);
        }
        fprintf(stderr, "[MBLUR-TEST] start: brake %u ms, %d shots\n", s_mbt_brake, s_mbt_n);
    }
    if (s_mbt_pending) {                /* the reference frame of the pair */
        int shot = s_mbt_pending;
        s_mbt_pending = 0;
        return shot;
    }
    if (s_mbt_next < s_mbt_n && t - s_mbt_t0 >= s_mbt_shots[s_mbt_next]) {
        fprintf(stderr, "[MBLUR-TEST] shot %02d t=%.0f speed %.1f/frame (%.0f km/h) reprojection %.2f px\n",
                s_mbt_next, t - s_mbt_t0, s_mb_speed, s_mb_speed * 60.0 * 0.036, s_mb_px);
        s_mb_off_next = 1;              /* the next frame: no pass */
        s_mbt_pending = s_mbt_next * 2 + 2;
        return s_mbt_next++ * 2 + 1;
    }
    return 0;
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
    progs_flip();
    scene_flip();
    s_draw_total = s_draw_no;
    s_draw_no = 0;
    j.shot = mbtest_flip();
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
    s_cam.valid = 0;
    s_scene_sent = s_scene_ran = 0;
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
