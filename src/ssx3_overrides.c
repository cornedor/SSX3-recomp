/**
 * SSX 3 - hand-written replacements for generated functions.
 *
 * Every sub_XXXXXXXX defined here replaces the generated body: the pipeline is
 * run with --exclude-manual src/ssx3_overrides.c, which skips generating any
 * function this file defines, so direct calls link to these.
 *
 * This file includes recomp_types.h rather than declaring the registers itself
 * the way recomp_manual.c does: the registers are thread-local, and a plain
 * `extern uint32_t g_eax` names the TLS template, not the running thread's copy.
 */
#ifdef _WIN32
#include <windows.h>
#else
#include "platform/win32_compat.h"
#endif
#include "recomp/gen/recomp_types.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 64-bit compare-and-swap on both hosts: true if *p was old and is now nu. */
#ifdef _WIN32
#define CAS64(p, old, nu) (InterlockedCompareExchange64((volatile LONG64 *)(p), (LONG64)(nu), \
                                                       (LONG64)(old)) == (LONG64)(old))
#else
#define CAS64(p, old, nu) __sync_bool_compare_and_swap((p), (old), (nu))
#endif

/*
 * sub_00183EBB - XAPI startup media check (XDK 5558 library code).
 *
 * When the certificate asks for it (cert+0x1D8 bit 0, which SSX 3 sets), this
 * reads the disc's security sector with READ DVD STRUCTURE and hands it to
 * sub_00183E04, which checks the title ID, SHA-1s it and verifies Microsoft's
 * RSA signature with XcVerifyPKCS1Signature against XePublicKeyData. Five
 * failures and the caller reboots through HalReturnToFirmware.
 *
 * That sector is pressed onto the physical disc and is not in a file-level
 * rip, and there is no drive here to read it from, so the check cannot pass.
 * Return what the function itself returns when the certificate does not ask
 * for the check (0x00183EFF: xor eax, eax): STATUS_SUCCESS.
 *
 * cdecl, no parameters: pop the return address the caller pushed, nothing else.
 */
void sub_00183EBB(void)
{
    g_eax = 0;
    g_esp += 4;
}

/*
 * CRT math through the x87 transcendental dispatcher, replaced natively.
 *
 * sub_00213850 is exp(double) and sub_00212C18 is fmod(double, double): cdecl,
 * arguments on the stack, result pushed on the x87 stack. Both reach the CRT's
 * hand-written dispatcher (ctrandisp1/ctrandisp2 -> trandisp*, a computed
 * `jmp [ebx]` into shared tails), and the lifted path returns with the
 * caller's ebx replaced by stale stack data (0x000D1284) -- most likely
 * ctrandisp1's `pop ebx` reads one slot off because a tail leaves the guest
 * esp 4 high (not yet verified). RECOMP_ABI_CHECK names it:
 * "[ABI] sub_00213850: ebx 00000000->000D1284". The caller that broke is the
 * state machine sub_001AD880, which keeps 0 in ebx across its loop: through
 * the loading screen's sub_001AD7B0 -> exp(), its render test
 * `cmp [esi+0x38], bl` stopped matching, the per-state update counter never
 * reset, and the "Loading..." fade never finished.
 *
 * These are the only two stubs that reach those dispatchers.
 */
static double arg_double(uint32_t va)
{
    union { uint32_t u[2]; double d; } v;
    v.u[0] = MEM32(va);
    v.u[1] = MEM32(va + 4);
    return v.d;
}

static void fp_push_result(double v)
{
    g_fp_top = (g_fp_top + 7) & 7;
    g_fp_stack[g_fp_top] = v;
}

/* double exp(double) */
void sub_00213850(void)
{
    fp_push_result(exp(arg_double(g_esp + 4)));
    g_esp += 4;
}

/* double fmod(double, double) */
void sub_00212C18(void)
{
    fp_push_result(fmod(arg_double(g_esp + 4), arg_double(g_esp + 12)));
    g_esp += 4;
}

/*
 * DirectSound effects-image download (sub_0025BD81, XDK DSOUND): after
 * copying the image it posts command 3 to the GP DSP at
 * *(*(*(this+8)+0x10)) + 0x810 and spins until the DSP clears it (guest
 * 0x0025BEE6). The DSP is a stub, so the toolkit's DSP ack clears it -- this
 * registers the word from the object itself instead of a fixed address
 * (RECOMP_DSP_ACK=0x83818810 depended on allocation order).
 */
extern void sub_0025BD81_gen(void);
extern int xbox_DspAckAdd(uint32_t va);

void sub_0025BD81(void)
{
    uint32_t p = g_ecx;
    if (p) p = MEM32(p + 8);
    if (p) p = MEM32(p + 0x10);
    if (p) p = MEM32(p);
    if (p)
        xbox_DspAckAdd(p + 0x810);
    sub_0025BD81_gen();
}

/*
 * Lens flare occlusion (sub_001F8760, called only by LensFX's sub_00152A70):
 * polls D3D's GetVisibilityTestResult up to 1M times until the GPU has
 * written the report, so the main thread waits every frame for the executor
 * to reach it -- title and GPU never overlap (measured: ~80% of the main
 * thread in this spin in a race). Its own no-wait path answers "not ready,
 * 0.0" = flare hidden. The Vulkan backend counts no visible pixels, so the
 * answer there is 0.0 either way; set by main.c when it is installed.
 * Args: (char *pending, index, total, char wait), stdcall.
 */
/*
 * Widescreen (RECOMP_WIDESCREEN): the main camera's projection is D3DX
 * PerspectiveFovLH (0x0024EA13, called only by the renderer's camera setup
 * sub_001EAB40 with aspect = viewport width / height = 640/480). Times
 * the target's shape / (4/3) it is that wide (16:9, 32:9, ...): the game
 * renders a wider view (its frustum, so its culling, follows the matrix)
 * squeezed into 640x480, which the Vulkan renderer then stretches to a
 * target of that shape. Args (stdcall): out, fovy, aspect, zn, zf.
 */
extern void sub_0024EA13_gen(void);

/* The target's shape (width / height; 4/3 = no widescreen). Starts as
 * RECOMP_ASPECT (main.c resolves RECOMP_WIDESCREEN into it); with
 * RECOMP_WIDESCREEN=auto the Vulkan backend changes it at a flip when the
 * window is resized (ssx3_SetAspect), so camera and target change together. */
static volatile float s_aspect = -1.0f;

float ssx3_Aspect(void)
{
    if (s_aspect < 0.0f) {
        const char *e = getenv("RECOMP_ASPECT");
        double a = e ? atof(e) : 0;
        s_aspect = a > 4.0 / 3.0 + 0.01 ? (float)a : 4.0f / 3.0f;
    }
    return s_aspect;
}

void ssx3_SetAspect(float a)
{
    s_aspect = a > 4.0f / 3.0f + 0.01f ? a : 4.0f / 3.0f;
}

/* Widescreen applies in game only (races, free-roam: app state vtable
 * 00299D7C). The frontend's art, loading screens and movies are designed
 * for 4:3: they keep the 4:3 camera and are shown pillarboxed. */
/* The app state is ingame (races, free-roam: vtable 00299D7C). */
int ssx3_InGame(void)
{
    uint32_t app = MEM32(0x003A930Cu), cur;
    if (app < 0x10000u)
        return 0;
    cur = MEM32(app + 4);
    return cur >= 0x10000u && MEM32(cur) == 0x00299D7Cu;
}

int ssx3_WidescreenNow(void)
{
    if (ssx3_Aspect() <= 4.0f / 3.0f + 0.01f)
        return 0;
    return ssx3_InGame();
}

extern float ssx3_CamFovScale(void);

void sub_0024EA13(void)
{
    float fk = ssx3_CamFovScale();
    if (fk != 1.0f && ssx3_InGame()) {   /* the race camera's speed FOV (camera_mode.c) */
        uint32_t bits = MEM32(g_esp + 8);
        float fovy;
        memcpy(&fovy, &bits, 4);
        fovy = 2.0f * atanf(tanf(fovy * 0.5f) * fk);
        memcpy(&bits, &fovy, 4);
        MEM32(g_esp + 8) = bits;
    }
    if (ssx3_WidescreenNow()) {
        uint32_t bits = MEM32(g_esp + 12);
        float aspect;
        memcpy(&aspect, &bits, 4);
        aspect *= ssx3_Aspect() * 0.75f;
        memcpy(&bits, &aspect, 4);
        MEM32(g_esp + 12) = bits;
    }
    sub_0024EA13_gen();
}

/*
 * Debug quick start menu (RECOMP_DEBUG_MENU=1). Every retail build keeps
 * EA's developer "quick start" menu (game path, mode, event, track,
 * character, AI riders, playlist, "Real Front End"). The known Xbox PAL code
 * 0009C7C2 = 01 patches the immediate of `mov dword ptr [esi+0x18], 0` in
 * this title-screen handler (taken when the message id [obj+0x38] matches
 * the "tReal" hash) to 1. Recompiled, that byte is C code, so the wrapper
 * does the same: a sentinel in obj+0x18, the original handler, and if it
 * stored its 0, a 1 instead. Nothing in the handler reads +0x18.
 */
extern void sub_0009C7A0_gen(void);

void sub_0009C7A0(void)
{
    static int on = -1;
    uint32_t obj = MEM32(g_esp + 4), old = 0;
    if (on < 0) {
        const char *e = getenv("RECOMP_DEBUG_MENU");
        on = e && *e && *e != '0';
        if (on) {
            fprintf(stderr, "[DEBUGMENU] quick start menu enabled (0009C7C2 = 01)\n");
            fflush(stderr);
        }
    }
    if (on && obj >= 0x10000u) {
        old = MEM32(obj + 0x18);
        MEM32(obj + 0x18) = 0x5EADBEEFu;
    }
    sub_0009C7A0_gen();
    if (on && obj >= 0x10000u) {
        if (MEM32(obj + 0x18) == 0) {
            MEM32(obj + 0x18) = 1;
            fprintf(stderr, "[DEBUGMENU] title handler: quick start selected (obj %08X)\n", obj);
            fflush(stderr);
        } else if (MEM32(obj + 0x18) == 0x5EADBEEFu) {
            MEM32(obj + 0x18) = old;
        }
    }
}

/* In-game screen opener sub_000C4C20(type), cdecl: RECOMP_STATION_TRACE=1
 * logs each call with the type and the guest return addresses on the stack
 * (who opened it: 0x1F = "enter the lodge?"). */
extern void sub_000C4C20_gen(void);

void sub_000C4C20(void)
{
    static int on = -1;
    if (on < 0) on = getenv("RECOMP_STATION_TRACE") != NULL;
    if (on) {
        int k;
        fprintf(stderr, "[INGAMESCREEN] open 0x%X from", MEM32(g_esp + 4));
        for (k = 0; k < 24; k++) {
            uint32_t w = MEM32(g_esp + k * 4);
            if (w >= 0x00011000u && w < 0x00280000u)
                fprintf(stderr, " %08X", w);
        }
        fprintf(stderr, "\n");
        fflush(stderr);
    }
    sub_000C4C20_gen();
}

/*
 * Station gate objects (lodge / transport): sub_000D5A60 is their per-frame
 * method; +0x10 state (0 = rider at the gate -> prompt), +0x14 kind (0 =
 * lodge -> in-game screen 0x1F "enter the lodge?", 2 = screen 0x21),
 * +0x0C/+0x1C flags, +0x18 a station id. RECOMP_STATION_TRACE=1 logs each
 * object's fields whenever they change.
 */
extern void sub_000D5A60_gen(void);

void sub_000D5A60(void)
{
    static int on = -1;
    static uint32_t objs[16], last[16][4];
    static int n;
    uint32_t o = g_ecx;
    if (on < 0) on = getenv("RECOMP_STATION_TRACE") != NULL;
    sub_000D5A60_gen();
    if (on && o >= 0x10000u) {
        int i;
        uint32_t v[4] = {MEM32(o + 0x0C), MEM32(o + 0x10), MEM32(o + 0x14), MEM32(o + 0x18)};
        for (i = 0; i < n && objs[i] != o; i++)
            ;
        if (i == n && n < 16) { objs[n] = o; memset(last[n], 0xFF, sizeof last[n]); n++; }
        if (i < n && memcmp(last[i], v, sizeof v)) {
            memcpy(last[i], v, sizeof v);
            fprintf(stderr, "[STATION] obj %08X +0C %08X state %u kind %u id %08X +1C %02X\n",
                    o, v[0], v[1], v[2], v[3], MEM8(o + 0x1C));
            fflush(stderr);
        }
    }
}

/*
 * "Go to the frontend" (sub_000E1B90, this = app): builds the real frontend
 * only if app+0x60 is set, else EA's DebugMenu. A debug quick start leaves it
 * 0, so the lodge prompt's Yes (sub_000B58C0: frontend start screen
 * [0x36843C] = 0x27, end the event) came back to the DebugMenu. With the
 * debug menu on and a lodge pending, take the real frontend: it opens on the
 * lodge (sub_00080A70) and stays the frontend from then on.
 */
extern void sub_000E1B90_gen(void);

void sub_000E1B90(void)
{
    static int on = -1;
    uint32_t app = g_ecx;
    if (on < 0) {
        const char *e = getenv("RECOMP_DEBUG_MENU");
        on = e && *e && *e != '0';
    }
    if (on && app >= 0x10000u && !MEM8(app + 0x60) && MEM32(0x0036843Cu) == 0x27) {
        MEM8(app + 0x60) = 1;
        fprintf(stderr, "[DEBUGMENU] lodge requested: real frontend from now on\n");
        fflush(stderr);
    }
    sub_000E1B90_gen();
}

/*
 * 30 Hz mode. The app loop (sub_001AD880) renders after every 60 Hz update
 * unless app+0x38 is set; then it renders only once two updates have run --
 * exactly 30 FPS, the game logic still at 60. The in-game update
 * (sub_000CC8F0, this = the in-game state) sets it on every update: always
 * with more than one player (split screen), else when slot 6 of the table at
 * 0x3992D0 (sub_00148CB0(6)) reads above 0.9 -- which happens in stretches of
 * a course, e.g. Metro-City's glowing tunnel. A choice made for the console's
 * GPU (xemu shows the same drop). Cleared after the update, so the game
 * renders every update; RECOMP_FPS30=1 keeps the game's own choice. Each
 * change of the game's choice is logged as [FPS30].
 */
extern void sub_000CC8F0_gen(void);
extern void ssx3_CamPreUpdate(void);
extern void ssx3_CamPostUpdate(uint32_t state);

void sub_000CC8F0(void)
{
    static int keep = -1, last = -1;
    uint32_t self = g_ecx, app;
    int want;
    if (keep < 0) {
        const char *e = getenv("RECOMP_FPS30");
        keep = e && *e && *e != '0';
    }
    ssx3_CamPreUpdate();                  /* the modern race camera: camera_mode.c */
    sub_000CC8F0_gen();
    ssx3_CamPostUpdate(self);
    app = MEM32(0x003A930Cu);
    if (app < 0x10000u)
        return;
    want = MEM8(app + 0x38) != 0;
    if (want != last) {
        uint32_t players = 0;
        if (self >= 0x10000u && MEM32(self + 0x84) >= 0x10000u)
            players = MEM32(MEM32(self + 0x84) + 0x10);
        fprintf(stderr, "[FPS30] t=%lu game wants %s (players %u)%s\n",
                GetTickCount(), want ? "30 Hz" : "60 Hz", players,
                want && !keep ? " -- overridden, rendering at 60" : "");
        fflush(stderr);
        last = want;
    }
    if (!keep)
        MEM8(app + 0x38) = 0;
}

/*
 * RECOMP_ACTION_TRACE=1: EA's action-map queries -- sub_001B1460 (bool:
 * is action N active) and sub_001B1410 (float: action N's value), both
 * thiscall (this = map definition) with (input state, action id). Once a
 * second each distinct action id queried, with how often and its last
 * non-zero result -- which inputs a screen reads and whether a press
 * reaches them.
 */
extern void sub_001B1460_gen(void);
extern void sub_001B1410_gen(void);
static int s_act_on = -1;
static struct { uint32_t act, state; unsigned n, hits; float last; int isf; } s_act[64];
static int s_nact;
static DWORD s_act_t;

static void act_note(uint32_t state, uint32_t act, float v, int isf)
{
    int i;
    DWORD now = GetTickCount();
    for (i = 0; i < s_nact && !(s_act[i].act == act && s_act[i].isf == isf && s_act[i].state == state); i++)
        ;
    if (i == s_nact && s_nact < 64) {
        s_act[i].act = act; s_act[i].isf = isf; s_act[i].state = state;
        s_act[i].n = s_act[i].hits = 0; s_act[i].last = 0; s_nact++;
    }
    if (i < s_nact) {
        s_act[i].n++;
        if (v != 0.0f) { s_act[i].hits++; s_act[i].last = v; }
    }
    if (now - s_act_t >= 1000) {
        s_act_t = now;
        fprintf(stderr, "[ACTION]");
        for (i = 0; i < s_nact; i++)
            fprintf(stderr, " %s%X@%08X x%u hit%u(%g)", s_act[i].isf ? "f" : "b", s_act[i].act,
                    s_act[i].state, s_act[i].n, s_act[i].hits, s_act[i].last);
        fprintf(stderr, "\n");
        fflush(stderr);
        s_nact = 0;
    }
}

void sub_001B1460(void)
{
    uint32_t state = MEM32(g_esp + 4), act = MEM32(g_esp + 8);
    if (s_act_on < 0) s_act_on = getenv("RECOMP_ACTION_TRACE") != NULL;
    sub_001B1460_gen();
    if (s_act_on) act_note(state, act, (float)g_eax, 0);
}

void sub_001B1410(void)
{
    uint32_t state = MEM32(g_esp + 4), act = MEM32(g_esp + 8);
    if (s_act_on < 0) s_act_on = getenv("RECOMP_ACTION_TRACE") != NULL;
    sub_001B1410_gen();
    if (s_act_on) {
        /* the float result is st(0) on the x87 stack (not popped) */
        act_note(state, act, (float)g_fp_stack[g_fp_top & 7], 1);
    }
}

extern void sub_001F8760_gen(void);
int ssx3_lensfx_nowait;

void sub_001F8760(void)
{
    if (ssx3_lensfx_nowait)
        MEM8(g_esp + 16) = 0;
    sub_001F8760_gen();
}

/*
 * EA job queues (RECOMP_JOB_TRACE=1): the 43% race-load stall leaves a
 * world-stream job unrun with every worker asleep. The queue is
 * {count +0, flags +4, head +8, tail +0xC, compare fn +0x10, ctx +0x14,
 * RTL_CRITICAL_SECTION +0x18}; sub_0017BE60(queue, job) inserts in priority
 * order, sub_0017BDA0(queue) pops the head. Each call goes into a ring that
 * every thread dump prints (ssx3_JobTraceDump), so a stall shows its last
 * few thousand queue operations.
 */
typedef struct { DWORD ms, tid; uint32_t op, queue, job, prio, count; } JobEv;
#define JOB_RING 4096
static JobEv s_jobs[JOB_RING];
static volatile LONG s_job_n;
static int s_job_on = -1;

static void job_note(uint32_t op, uint32_t queue, uint32_t job)
{
    LONG i;
    if (s_job_on < 0)
        s_job_on = getenv("RECOMP_JOB_TRACE") != NULL;
    if (!s_job_on)
        return;
    i = InterlockedIncrement(&s_job_n) - 1;
    s_jobs[i % JOB_RING].ms    = GetTickCount();
    s_jobs[i % JOB_RING].tid   = GetCurrentThreadId();
    s_jobs[i % JOB_RING].op    = op;
    s_jobs[i % JOB_RING].queue = queue;
    s_jobs[i % JOB_RING].job   = job;
    s_jobs[i % JOB_RING].prio  = job ? MEM8(job + 0x11) : 0xFFFFFFFFu;
    s_jobs[i % JOB_RING].count = queue ? MEM32(queue) : 0;
}

void ssx3_JobTraceDump(void)
{
    LONG n = s_job_n, i, first;
    if (s_job_on <= 0 || n == 0)
        return;
    first = n > JOB_RING ? n - JOB_RING : 0;
    fprintf(stderr, "[JOBS] last %ld of %ld queue operations (ms tid op queue job prio count):\n",
            n - first, n);
    for (i = first; i < n; i++) {
        const JobEv *e = &s_jobs[i % JOB_RING];
        fprintf(stderr, "[JOB] %lu %lu %s %08X %08X %u %u\n", e->ms, e->tid,
                e->op == 1 ? "ins" : e->op == 2 ? "pop" : "pop0",
                e->queue, e->job, e->prio, e->count);
    }
    fflush(stderr);
}

extern void sub_0017BE60_gen(void);
extern void sub_0017BDA0_gen(void);

void sub_0017BE60(void)
{
    uint32_t queue = MEM32(g_esp + 4), job = MEM32(g_esp + 8);
    sub_0017BE60_gen();
    job_note(1, queue, job);
}

void sub_0017BDA0(void)
{
    uint32_t queue = MEM32(g_esp + 4);
    sub_0017BDA0_gen();
    job_note(g_eax ? 2 : 3, queue, g_eax);
}

/*
 * World streamer slot check (sub_0020CAB0(i), thiscall: ecx = streamer,
 * [esp+4] = slot): returns [this + i*24 + 0x3EC] == 3. Crashes 3 runs in 6
 * (with RECOMP_BLOCK_TRACE on) with i = 0x171B2880 -- a garbage slot index
 * from the streamer's own lists; the 43% load stall is the same object.
 * RECOMP_STREAM_CHECK=1: an index past MAX_SLOTS is logged with the guest
 * call chain and the object, and answered "not ready" (0) instead of read.
 */
/* D3D's gamma-ramp upload (SetGammaRamp's hardware half, from the vblank
 * path and the display reset): 256 R, then G, then B bytes at [esp+4],
 * written through the NV2A's VGA palette port, which our MMIO does not keep.
 * Hand the ramp to the presenter (toolkit fb_gamma.c), then run the
 * original. SSX 3's ramp darkens the mid-tones; without it the picture is
 * washed out next to the console. */
extern void sub_00239B76_gen(void);
extern void xbox_FramebufferSetGammaRamp(const uint8_t *r, const uint8_t *g, const uint8_t *b);

void sub_00239B76(void)
{
    uint32_t ramp = MEM32(g_esp + 4);
    if (ramp) {
        const uint8_t *p = (const uint8_t *)&MEM8(ramp);
        xbox_FramebufferSetGammaRamp(p, p + 256, p + 512);
    }
    sub_00239B76_gen();
}

/* RECOMP_UI_TRACE=1: the frontend's list widgets (EA UI lib). sub_00205D40
 * selects logical item <index> of a list (this+0x96 items shown through
 * this+0x97 child rows, scrolled by this+0x98); sub_0008C540 is a screen's
 * "restore the remembered item" (index from the table at 0x368448 by screen
 * id this+0xC). The Options screen crashed there on an item lookup that
 * returned NULL. */
extern void sub_00205D40_gen(void);
extern void sub_0008C540_gen(void);
static int ui_trace_on(void)
{
    static int on = -1;
    if (on < 0)
        on = getenv("RECOMP_UI_TRACE") != NULL;
    return on;
}

void sub_00205D40(void)
{
    if (ui_trace_on()) {
        uint32_t self = g_ecx;
        fprintf(stderr, "[UI] list %08X vt %08X select %u | flags14 %08X sel %u items %u rows %u scroll %u"
                " focus %08X saved %u parent %08X | ret %08X\n",
                self, MEM32(self), MEM8(g_esp + 4), MEM32(self + 0x14), MEM8(self + 0x95),
                MEM8(self + 0x96), MEM8(self + 0x97), MEM8(self + 0x98), MEM32(self + 0xA0),
                MEM8(self + 0xA4), MEM32(self + 0x5C), MEM32(g_esp));
        fflush(stderr);
    }
    sub_00205D40_gen();
}

/* Every frontend screen's state change ends in sub_00202460 (ecx = the
 * screen; it passes the change to the screen's children). The state is
 * bits 8-13 of the screen's word +0x1C; 5 = active. With RECOMP_UI_TRACE,
 * log each screen that becomes active: vtable and id (+0xC) name the page,
 * which the menu scripts navigate by. Also published as game_state.txt's
 * screen fields (game_state.c). (A wrapper on the screen manager's branch
 * sub_00204400 never ran: it is a jump target inside its parent.) */
extern void sub_00202460_gen(void);
volatile uint32_t g_ssx3_ui_screen_vt, g_ssx3_ui_screen_id, g_ssx3_ui_screen_ptr;
#define SSX3_HUD_SCREEN_VT 0x00293000u  /* in-game HUD: re-activates all the time */

void sub_00202460(void)
{
    uint32_t scr = g_ecx;
    {
        /* RECOMP_UI_TRACE=2: every screen state change, with the screen's
         * first children (the widget list at +0x24: head [+0x24+0xC]). */
        static int verbose = -1;
        if (verbose < 0) {
            const char *e = getenv("RECOMP_UI_TRACE");
            verbose = e && atoi(e) >= 2;
        }
        if (verbose && scr) {
            uint32_t head = MEM32(scr + 0x24 + 0xC), n = head, k;
            fprintf(stderr, "[UI2] screen %08X vt %08X id %d state %u children:", scr, MEM32(scr),
                    (int)MEM32(scr + 0xC), (MEM16(scr + 0x1C) >> 8) & 0x3F);
            for (k = 0; k < 12 && n; k++) {
                uint32_t next = MEM32(n + 8);
                fprintf(stderr, " %08X(vt %08X)", n, MEM32(n));
                if (next == n || next == head)
                    break;
                n = next;
            }
            fprintf(stderr, "\n");
            fflush(stderr);
        }
    }
    if (scr && ((MEM16(scr + 0x1C) >> 8) & 0x3F) == 5 && MEM32(scr) != SSX3_HUD_SCREEN_VT) {
        uint32_t vt = MEM32(scr), id = MEM32(scr + 0xC);
        g_ssx3_ui_screen_ptr = scr;
        if (vt != g_ssx3_ui_screen_vt || id != g_ssx3_ui_screen_id) {
            g_ssx3_ui_screen_vt = vt;
            g_ssx3_ui_screen_id = id;
            if (ui_trace_on()) {
                fprintf(stderr, "[UI] active screen %08X vt %08X id %d\n", scr, vt, (int)id);
                fflush(stderr);
            }
        }
    }
    sub_00202460_gen();
}

void sub_0008C540(void)
{
    if (ui_trace_on()) {
        uint32_t self = g_ecx;
        fprintf(stderr, "[UI] screen %08X vt %08X id %d restore focus | ret %08X\n",
                self, MEM32(self), (int)(int8_t)MEM8(self + 0xC), MEM32(g_esp));
        fflush(stderr);
    }
    sub_0008C540_gen();
}

#define STREAM_MAX_SLOTS 4096u
extern void sub_0020CAB0_gen(void);

void sub_0020CAB0(void)
{
    static int on = -1;
    static volatile LONG reports;
    uint32_t i = MEM32(g_esp + 4), self = g_ecx;
    if (on < 0)
        on = getenv("RECOMP_STREAM_CHECK") != NULL;
    if (on && i >= STREAM_MAX_SLOTS) {
        if (InterlockedIncrement(&reports) <= 5) {
            int k;
            fprintf(stderr, "[STREAM] bad slot %08X (this %08X) t%lu | ret", i, self,
                    (unsigned long)GetCurrentThreadId());
            for (k = 0; k < 24; k++) {          /* return addresses into .text */
                uint32_t w = MEM32(g_esp + 4 * k);
                if (w >= 0x00011000u && w < 0x00280000u)
                    fprintf(stderr, " %08X", w);
            }
            fprintf(stderr, "\n[STREAM] regs eax %08X ebx %08X edx %08X esi %08X edi %08X ebp %08X\n",
                    g_eax, g_ebx, g_edx, g_esi, g_edi, g_ebp);
            for (k = 0; k < 0x400; k += 32)
                fprintf(stderr, "[STREAM] +%03X %08X %08X %08X %08X %08X %08X %08X %08X\n", k,
                        MEM32(self + k), MEM32(self + k + 4), MEM32(self + k + 8),
                        MEM32(self + k + 12), MEM32(self + k + 16), MEM32(self + k + 20),
                        MEM32(self + k + 24), MEM32(self + k + 28));
            fflush(stderr);
        }
        g_eax = 0;
        g_esp += 8;                             /* ret 4 */
        return;
    }
    sub_0020CAB0_gen();
}

/*
 * Who is inside the world streamer. Two guest threads drive it: the main
 * thread's update (sub_000CBB10 -> pump sub_0020AF20, and sub_0020D4C0) and
 * the loader thread's poll loop (sub_000CC0A0 -> the same pump, and the world
 * reload sub_0020B3F0). The reload frees the old world file first
 * (sub_0020D8F0 -> sub_0020D850) -- which holds the request list the pump
 * walks ([S+0x18] = file+0x50, count at [[S+0x14]+8]) -- and leaves those
 * two pointers dangling until sub_0020B8B0 re-points them after the new file
 * has been read, a slow kernel call. A pump on another thread in that window
 * reads freed memory: garbage slot indices (sub_0020CAB0 crash) and lost
 * requests (the 43% stall). Game code also enters via sub_0020C4A0/C520,
 * and the resource poller sub_000E0490 posts requests (sub_0020B700 /
 * sub_0020B7B0) by writing the per-entry state word the pass reads and later
 * overwrites -- a request posted mid-pass can be overwritten and lost.
 *
 * RECOMP_STREAM_CHECK=1 logs every entry made while another thread is inside
 * ([STREAM-OVERLAP] with both names); RECOMP_STREAM_SERIAL=1 makes the
 * second thread wait (yielding the guest CPU) until the first has left, at
 * most 5 s.
 */
static volatile DWORD s_strm_owner;
static const char    *s_strm_what;
static int            s_strm_depth;

static void strm_enter(const char *what)
{
    static int check = -1, serial = -1;
    static volatile LONG reports;
    DWORD me = GetCurrentThreadId();
    if (check < 0) {
        const char *s = getenv("RECOMP_STREAM_SERIAL");
        serial = s && s[0] == '1';
        check = serial || getenv("RECOMP_STREAM_CHECK") != NULL;
    }
    if (!check)
        return;
    if (s_strm_owner && s_strm_owner != me) {
        if (InterlockedIncrement(&reports) <= 40)
            fprintf(stderr, "[STREAM-OVERLAP] t=%lu %s on t%lu while %s on t%lu%s\n",
                    (unsigned long)GetTickCount(), what, (unsigned long)me, s_strm_what,
                    (unsigned long)s_strm_owner, serial ? " (waiting)" : "");
        if (serial) {
            DWORD t0 = GetTickCount();
            while (s_strm_owner && s_strm_owner != me && GetTickCount() - t0 < 5000)
                recomp_loop_yield();
            if (s_strm_owner && s_strm_owner != me)
                fprintf(stderr, "[STREAM-OVERLAP] %s gave up waiting for %s\n", what, s_strm_what);
        }
    }
    if (s_strm_owner != me) {
        s_strm_owner = me;
        s_strm_what = what;
        s_strm_depth = 0;
    }
    s_strm_depth++;
}

static void strm_leave(void)
{
    if (s_strm_owner == GetCurrentThreadId() && --s_strm_depth <= 0) {
        s_strm_depth = 0;
        s_strm_owner = 0;
    }
}

extern void sub_0020AF20_gen(void);
extern void sub_000E0490_gen(void);
extern void sub_0020B3F0_gen(void);
extern void sub_0020D4C0_gen(void);
extern void sub_0020C4A0_gen(void);
extern void sub_0020C520_gen(void);

static volatile uint32_t s_strm_obj;   /* the streamer the pump last ran */

void sub_0020AF20(void)
{
    s_strm_obj = MEM32(g_ecx);
    strm_enter("pump");
    sub_0020AF20_gen();
    strm_leave();
}
void sub_0020B3F0(void)
{
    strm_enter("reload");
    sub_0020B3F0_gen();
    strm_leave();
}
void sub_0020D4C0(void)
{
    strm_enter("D4C0");
    sub_0020D4C0_gen();
    strm_leave();
}
void sub_0020C4A0(void)
{
    strm_enter("C4A0");
    sub_0020C4A0_gen();
    strm_leave();
}
void sub_0020C520(void)
{
    strm_enter("C520");
    sub_0020C520_gen();
    strm_leave();
}

void sub_000E0490(void)
{
    strm_enter("E0490");
    sub_000E0490_gen();
    strm_leave();
}

/*
 * Streaming pool (sub_0020C740 builds it per world: three block lists of
 * count x size from 0x3432E0.. -- 0 x 16 KB, 2 x 32 KB, 53 x 128 KB in the
 * image -- record per block 0x18 bytes: +0 memory, +4 size, +8 used, +0x10
 * in-use byte, +0x14 next free; list: +0 block size, +4 count, +8 free count,
 * +0xC records, +0x10 free head). sub_0020C6A0 pops a block with no empty
 * check: on an empty list it reads the next pointer from guest 0x14, installs
 * that as the free head and returns 0 -- the allocation then fails (the
 * streamer retries forever: the 43% stall) and every later pop hands out
 * garbage (suspected source of the streamer crashes). On the Xbox page 0 is
 * unmapped, so the title must never run out there.
 *
 * The wrapper logs [POOL] exhausted (list, caller) and returns 0 without
 * touching the list. RECOMP_STREAM_POOL_EXTRA=N gives each non-empty list N
 * more blocks when the pool is built.
 */
extern void sub_0020C6A0_gen(void);
extern void sub_0020C740_gen(void);
static volatile uint32_t s_pool_obj;

void sub_0020C6A0(void)
{
    uint32_t list = g_ecx;
    if (MEM32(list + 0x10) == 0) {
        static volatile LONG n;
        if (InterlockedIncrement(&n) <= 20) {
            fprintf(stderr, "[POOL] exhausted: list %08X (%u x 0x%X, free %d) caller %08X t%lu\n",
                    list, MEM32(list + 4), MEM32(list), (int)MEM32(list + 8), MEM32(g_esp),
                    (unsigned long)GetCurrentThreadId());
            fflush(stderr);
        }
        g_eax = 0;
        g_esp += 4;
        return;
    }
    sub_0020C6A0_gen();
}

void sub_0020C740(void)
{
    static int extra = -1;
    if (extra < 0) {
        const char *e = getenv("RECOMP_STREAM_POOL_EXTRA");
        extra = e ? atoi(e) : 0;
    }
    if (extra > 0) {
        static const uint32_t counts[3] = { 0x003432E0u, 0x003432ECu, 0x003432F8u };
        static int done;
        int k;
        if (!done++)
            for (k = 0; k < 3; k++)
                if (MEM32(counts[k]))
                    MEM32(counts[k]) += (uint32_t)extra;
    }
    s_pool_obj = g_ecx;
    sub_0020C740_gen();
    fprintf(stderr, "[POOL] streaming pool %08X: %u x 0x%X, %u x 0x%X, %u x 0x%X\n", s_pool_obj,
            MEM32(s_pool_obj + 4), MEM32(s_pool_obj), MEM32(s_pool_obj + 0x18), MEM32(s_pool_obj + 0x14),
            MEM32(s_pool_obj + 0x2C), MEM32(s_pool_obj + 0x28));
    fflush(stderr);
}

/*
 * Banks. Request entry i of the streamer owns bank i: sub_0020C9B0(i, kind)
 * creates its pool ([pool + 0x3C + i*4]) and bank object when the request
 * starts (entry state 1 -> 2), sub_0020D2A0(i) destroys both when the entry
 * is unloaded (3 -> 0). A streamed asset names its bank in the low byte of
 * its id; sub_0020D6B0(slot, type, id, size) places it and returns -1 when it
 * cannot -- which the reader retries forever (the 43% stall: the bank's pool
 * pointer was NULL). Logged: every failure ([BANK] place failed, with the
 * bank's entry state) and, with RECOMP_STREAM_CHECK=1, every create/destroy.
 */
extern void sub_0020D6B0_gen(void);
extern void sub_0020C9B0_gen(void);
extern void sub_0020D2A0_gen(void);

static int strm_check_on(void)
{
    static int on = -1;
    if (on < 0)
        on = getenv("RECOMP_STREAM_CHECK") != NULL || getenv("RECOMP_STREAM_SERIAL") != NULL;
    return on;
}

void sub_0020D6B0(void)
{
    uint32_t S = g_ecx, slot = MEM32(g_esp + 4), type = MEM32(g_esp + 8),
             id = MEM32(g_esp + 12), size = MEM32(g_esp + 16);
    sub_0020D6B0_gen();
    if (g_eax == 0xFFFFFFFFu) {
        static volatile LONG n;
        LONG k = InterlockedIncrement(&n);
        if (k <= 10 || k % 1000000 == 0) {
            uint32_t bank = id & 0xFF, mgr = S + 0x10;
            fprintf(stderr, "[BANK] place failed #%ld t=%lu: slot %u type %u id %06X bank %u size 0x%X"
                    " | bank entry state %u, pool %08X, bank obj %08X\n", (long)k,
                    (unsigned long)GetTickCount(), slot, type, id >> 8, bank, size,
                    bank < 89 ? MEM32(mgr + 0x14 + 8 * bank) : 0xFFFFFFFFu,
                    bank != 0xFF ? MEM32(MEM32(S + 0xC) + 0x3C + bank * 4) : 0,
                    bank != 0xFF ? MEM32(MEM32(S + 8) + bank * 4) : 0);
            fflush(stderr);
        }
    }
}

void sub_0020C9B0(void)
{
    if (strm_check_on())
        fprintf(stderr, "[BANK] create %u (kind %u) t=%lu\n", MEM32(g_esp + 4), MEM32(g_esp + 8),
                (unsigned long)GetTickCount());
    sub_0020C9B0_gen();
}

void sub_0020D2A0(void)
{
    if (strm_check_on())
        fprintf(stderr, "[BANK] destroy %u t=%lu ret %08X\n", MEM32(g_esp + 4),
                (unsigned long)GetTickCount(), MEM32(g_esp));
    sub_0020D2A0_gen();
}

/*
 * Chunk decompression (sub_0020DA80(dst, src, srclen) -> RefPack via
 * sub_0017C840, returns the decompressed size). The stall's first asset
 * header was garbage although the stream's first CBXS block decompresses to a
 * valid one (0x14000 bytes, type 24, bank 10): the reader returned 0xA16E.
 * With RECOMP_STREAM_CHECK=1 every call is logged ([CHUNK] src bytes, length,
 * result, first header) so the delivered block can be found in the file.
 */
extern void sub_0020DA80_gen(void);

void sub_0020DA80(void)
{
    uint32_t dst = MEM32(g_esp + 4), src = MEM32(g_esp + 8), len = MEM32(g_esp + 12);
    int log = strm_check_on();
    uint8_t head[16];
    int k;
    if (log)
        for (k = 0; k < 16; k++)
            head[k] = MEM8(src + k);
    sub_0020DA80_gen();
    if (log) {
        static volatile LONG n;
        if (InterlockedIncrement(&n) <= 4000) {
            fprintf(stderr, "[CHUNK] t=%lu src %08X len 0x%X -> 0x%X | src", (unsigned long)GetTickCount(),
                    src, len, g_eax);
            for (k = 0; k < 16; k++)
                fprintf(stderr, " %02X", head[k]);
            fprintf(stderr, " | hdr %08X %08X\n", MEM32(dst), MEM32(dst + 4));
            fflush(stderr);
        }
    }
}

/*
 * EA file layer and STRM request start, logged with RECOMP_STREAM_CHECK=1.
 * In a stall the world stream continued past the 'CEND' block that ends a
 * request (xsb +0x13B8000.. instead of the next request's +0x0): the next
 * request was read at the old file position. sub_0017A5A0(~fd, ?, buf, pos,
 * len) reads through EA's file table [0x39E884] (20-byte entries: +0 handle,
 * +4 cached position, +8 size, +0xC base in the archive) and seeks only when
 * it thinks it must; sub_00177990 (esi = manager) starts the next request.
 */
extern void sub_0017A5A0_gen(void);
extern void sub_00177990_gen(void);

void sub_0017A5A0(void)
{
    uint32_t fd = ~MEM32(g_esp + 4), pos = MEM32(g_esp + 12), len = MEM32(g_esp + 16);
    uint32_t tab = MEM32(0x0039E884u), ent = tab + fd * 20;
    uint32_t cached = tab ? MEM32(ent + 4) : 0, base = tab ? MEM32(ent + 12) : 0;
    int log = strm_check_on() && tab && fd < 64;
    int slot = -1;
    /* Always seek. EA caches each descriptor's file position and skips the
     * seek when it matches the wanted one, but several descriptors can share
     * one OS handle (BAM.big is fd 22 and the BAM.xsb inside it fd 23): a read
     * through one moves the handle's position under the other's cache, and the
     * next "sequential" read comes from the wrong place -- the world-stream
     * stall. Make the cached value never equal the wanted one so the seek is
     * always issued. RECOMP_EA_FORCE_SEEK=0 turns it off. */
    {
        static int force = -1;
        if (force < 0) {
            const char *e = getenv("RECOMP_EA_FORCE_SEEK");
            force = !(e && *e == '0');
        }
        if (force && tab && fd < MEM32(0x0039E880u) && MEM32(ent) != 0) {
            uint32_t want = MEM32(g_esp + 12);
            MEM32(ent + 4) = (base ? base - want : want) ^ 1;
        }
    }
    if (log) {
        /* Two threads inside the file layer on one OS handle: its seek and
         * read are separate kernel calls with no lock, so the second thread's
         * seek can land between the first's seek and read. */
        static volatile uint64_t inflight[16];   /* handle << 32 | tid, 0 = free */
        uint32_t handle = MEM32(ent), tid = (uint32_t)GetCurrentThreadId();
        uint64_t mine = ((uint64_t)handle << 32) | tid;
        int k;
        for (k = 0; k < 16; k++) {
            uint64_t o = inflight[k];
            if (o && (uint32_t)(o >> 32) == handle && (uint32_t)o != tid)
                fprintf(stderr, "[EAOVERLAP] t=%lu handle %08X: thread %u enters (fd %u pos 0x%X) while thread %u is inside\n",
                        (unsigned long)GetTickCount(), handle, tid, fd, pos, (uint32_t)o);
        }
        for (k = 0; k < 16; k++)
            if (!inflight[k] && CAS64(&inflight[k], 0, mine)) { slot = k; break; }
        sub_0017A5A0_gen();
        if (slot >= 0)
            inflight[slot] = 0;
    } else {
        sub_0017A5A0_gen();
    }

    if (log) {
        static volatile LONG n;
        if (InterlockedIncrement(&n) <= 20000) {
            fprintf(stderr, "[EAREAD] t=%lu t%lu fd %u pos 0x%X len 0x%X | cached 0x%X base 0x%X -> got 0x%X"
                    " cached now 0x%X\n", (unsigned long)GetTickCount(), (unsigned long)GetCurrentThreadId(),
                    fd, pos, len, cached, base, g_eax, MEM32(ent + 4));
            fflush(stderr);
        }
    }
}


/*
 * EA's STRM stream manager: hand-off between its I/O thread and the loader
 * thread. The console runs these without a thread switch in practice; here
 * every kernel call and loop head is a switch point, and in ~1 of 3 loads the
 * end of the world's first request (BAM.xsb, ASKY) is handed over with the
 * I/O side already reading on -- the load never completes. These entry points
 * keep the guest CPU (blocking waits still release it). OFF by default,
 * RECOMP_STRM_ATOMIC=1 enables: sub_00178280 calls the polling pull
 * (sub_00177F80), so wrapping it livelocks the load (stack seen in a stall).
 */
extern void xbox_NoPreemptEnter(void);
extern void xbox_NoPreemptLeave(void);
static void strm_note(char op, uint32_t m, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e);
static int strm_ok(uint32_t va);
static int strm_atomic(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_STRM_ATOMIC");
        on = e && *e && *e != '0';      /* opt-in: livelocks, see below */
    }
    return on;
}


/* [OVL] two guest threads inside the stream manager's entry points at once
 * (RECOMP_STREAM_CHECK=1). With the guest-CPU lock that only happens when a
 * switch lands inside one of them. */
static volatile uint64_t ovl_inflight[8];   /* func << 32 | tid, 0 = free */

static int ovl_enter(uint32_t func)
{
    uint32_t tid = (uint32_t)GetCurrentThreadId();
    int k, slot = -1;
    if (!strm_check_on())
        return -1;
    for (k = 0; k < 8; k++) {
        uint64_t o = ovl_inflight[k];
        if (o && (uint32_t)o != tid)
            fprintf(stderr, "[OVL] t=%lu thread %u enters %06X while thread %u is inside %06X\n",
                    (unsigned long)GetTickCount(), tid, func, (uint32_t)o, (uint32_t)(o >> 32));
    }
    for (k = 0; k < 8; k++)
        if (!ovl_inflight[k] && CAS64(&ovl_inflight[k], 0, ((uint64_t)func << 32) | tid)) { slot = k; break; }
    return slot;
}

static void ovl_leave(int slot)
{
    if (slot >= 0)
        ovl_inflight[slot] = 0;
}

extern void sub_001781B0_gen(void);

/* [SM] request state transitions (RECOMP_STREAM_CHECK=1): the I/O completion
 * callback (sub_001781B0) and the consumer's close/cancel (sub_001785D0). */
static void sm_name(uint32_t req, char *out)
{
    int c;
    for (c = 0; c < 23; c++) { uint8_t x = req ? MEM8(req + 0x14 + c) : 0; if (!x) break; out[c] = (char)x; }
    out[c] = 0;
}

void sub_001781B0(void)
{
    int a = strm_atomic();
    int log = strm_check_on();
    /* callback(op, 0, M): the scheduler pushes M, 0, 0 (0x0017848A). */
    uint32_t op = MEM32(g_esp + 4), M = MEM32(g_esp + 12), req = MEM32(M + 0x70);
    uint32_t before = req ? MEM32(req + 4) : 0xFF;
    uint32_t pos0 = MEM32(M + 0x180);
    int ov = ovl_enter(0x1781B0);
    strm_note('C', M, op, MEM32(M + 0x68), MEM32(M + 0x188), MEM32(M + 0x180), MEM32(M + 0x48));
    if (a) xbox_NoPreemptEnter();
    sub_001781B0_gen();
    if (a) xbox_NoPreemptLeave();
    strm_note('c', M, op, MEM32(M + 0x68), MEM32(M + 0x64), MEM32(M + 0x60), MEM32(M + 0x48));
    ovl_leave(ov);
    if (log && req) {
        uint32_t after = MEM32(req + 4);
        char nm[24];
        sm_name(req, nm);
        fprintf(stderr, "[SM] t=%lu t%lu cb   req %08X state %u->%u cur %08X pos 0x%X->0x%X '%s'\n",
                (unsigned long)GetTickCount(), (unsigned long)GetCurrentThreadId(), req, before, after,
                MEM32(M + 0x70), pos0, MEM32(M + 0x180), nm);
    }
}

extern void sub_001785D0_gen(void);

void sub_001785D0(void)
{
    uint32_t h = MEM32(g_esp + 4), id = MEM32(g_esp + 8), M = h ? MEM32(h) : 0;
    uint32_t req = M ? MEM32(M + 0x24) + (id & 0xFF) * 0x124 : 0;
    uint32_t before = req ? MEM32(req + 4) : 0xFF;
    char nm[24];
    int log = strm_check_on() && req;
    if (log) sm_name(req, nm);
    sub_001785D0_gen();
    if (log)
        fprintf(stderr, "[SM] t=%lu t%lu close req %08X state %u->%u cur %08X '%s'\n",
                (unsigned long)GetTickCount(), (unsigned long)GetCurrentThreadId(), req, before,
                MEM32(req + 4), MEM32(M + 0x70), nm);
}

extern void sub_00178280_gen(void);

void sub_00178280(void)                 /* schedule the next read (M, n) */
{
    int a = strm_atomic();
    uint32_t M = MEM32(g_esp + 4);
    int ov = ovl_enter(0x178280);
    strm_note('K', M, MEM32(M + 0x60), MEM32(M + 0x64), MEM32(M + 0x68), MEM32(M + 0x48), MEM32(M + 0x18C));
    if (a) xbox_NoPreemptEnter();
    sub_00178280_gen();
    if (a) xbox_NoPreemptLeave();
    strm_note('k', M, MEM32(M + 0x60), MEM32(M + 0x64), MEM32(M + 0x68), MEM32(M + 0x48), MEM32(M + 0x188));
    ovl_leave(ov);
}

extern void sub_00177760_gen(void);

/*
 * Chunk parser, on EA's file worker after each read. It publishes a chunk
 * under the lock, unlocks (0x001778A2), and only then re-reads the chunk's
 * tag from the ring (0x001778B2) to see whether it was the request's end
 * chunk ('CEND'). If the loader pulls and releases that chunk in between, the
 * release has overwritten the tag with -2 (0x00178575): the end is missed and
 * the worker keeps reading the bytes that follow on disk as if they belonged
 * to the request -- garbage assets, the 43% load stall. On the console the
 * worker keeps the CPU through an uncontended unlock; keep the guest CPU here
 * too. The parser does not poll, so this cannot livelock. Blocking lock waits
 * still release the CPU. RECOMP_STRM_PARSE_ATOMIC=0 turns it off.
 */
static int strm_parse_atomic(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_STRM_PARSE_ATOMIC");
        on = !(e && *e == '0');
    }
    return on;
}

void sub_00177760(void)                 /* scan new blocks; eax = manager */
{
    int a = strm_parse_atomic();
    uint32_t M = g_eax, scan = MEM32(M + 0x64), wr = MEM32(M + 0x68);
    uint32_t tag = strm_ok(scan) ? MEM32(scan) : 0, sz = strm_ok(scan) ? MEM32(scan + 4) : 0;
    int ov = ovl_enter(0x177760);
    if (a) xbox_NoPreemptEnter();
    sub_00177760_gen();
    if (a) xbox_NoPreemptLeave();
    ovl_leave(ov);
    strm_note('S', M, scan, wr, tag, sz, (g_eax << 28) | (MEM32(M + 0x64) & 0x0FFFFFFFu));
}

extern void sub_00177E10_gen(void);

void sub_00177E10(void)
{
    sub_00177E10_gen();   /* polls for the I/O thread: must stay preemptible */
}

extern void sub_00177F80_gen(void);

/* [CEND-RACE] (RECOMP_STREAM_CHECK=1): the loader pulls the current
 * request's end chunk while the parser (sub_00177760) is still inside on
 * another thread -- the window in which the end is missed. */
void sub_00177F80(void)                 /* next chunk of stream h */
{
    /* A consumer polls an empty stream every frame: an empty fetch is
     * recorded once per handle, not each time, so the ring keeps history. */
    static volatile uint32_t last_h[4], last_cur[4];
    uint32_t h = MEM32(g_esp + 4), M = h ? MEM32(h) : 0;
    uint32_t cur = strm_ok(h) ? MEM32(h + 12) : 0, avail = strm_ok(h) ? MEM32(h + 8) : 0;
    int k, slot = -1, quiet = 0;
    for (k = 0; k < 4; k++)
        if (last_h[k] == h) { slot = k; break; }
    if (slot < 0)
        for (k = 0; k < 4; k++)
            if (!last_h[k]) { slot = k; last_h[k] = h; break; }
    quiet = slot >= 0 && avail == 0 && last_cur[slot] == cur;
    if (!quiet)
        strm_note('g', M, h, strm_ok(h) ? MEM32(h + 4) : 0, avail, cur, strm_ok(cur) ? MEM32(cur + 4) : 0);
    sub_00177F80_gen();   /* polls for the I/O thread: must stay preemptible */
    if (!quiet || g_eax)
        strm_note('G', M, h, g_eax, strm_ok(h) ? MEM32(h + 8) : 0, strm_ok(h) ? MEM32(h + 12) : 0,
                  strm_ok(M) ? MEM32(M + 0x60) : 0);
    if (slot >= 0)
        last_cur[slot] = strm_ok(h) ? MEM32(h + 12) : 0;
    if (strm_check_on() && g_eax && M) {
        uint32_t req = MEM32(M + 0x70), tid = (uint32_t)GetCurrentThreadId();
        int k;
        if (req && MEM32(g_eax) == MEM32(req + 0x11C))
            for (k = 0; k < 8; k++) {
                uint64_t o = ovl_inflight[k];
                if (o && (uint32_t)(o >> 32) == 0x177760 && (uint32_t)o != tid)
                    fprintf(stderr, "[CEND-RACE] t=%lu t%u pulled end chunk %08X of req %08X while t%u parses\n",
                            (unsigned long)GetTickCount(), tid, g_eax, req, (uint32_t)o);
            }
    }
}

void sub_00177990(void)
{
    uint32_t M = g_esi, req = MEM32(M + 0x70);
    if (strm_check_on()) {
        char a[40], b[40];
        int c;
        for (c = 0; c < 39; c++) { uint8_t x = req ? MEM8(req + 0x14 + c) : 0; if (!x) break; a[c] = (char)x; }
        a[c] = 0;
        for (c = 0; c < 39; c++) { uint8_t x = MEM8(M + 0x7C + c); if (!x) break; b[c] = (char)x; }
        b[c] = 0;
        fprintf(stderr, "[STRM] t=%lu t%lu ret %08X start: M %08X busy %u req %08X state %u pos 0x%X tag %08X '%s'"
                " | open fd %08X '%s' pos 0x%X\n", (unsigned long)GetTickCount(), (unsigned long)GetCurrentThreadId(), MEM32(g_esp), M, MEM32(M + 0x48),
                req, req ? MEM32(req + 4) : 0, req ? MEM32(req + 0x118) : 0, req ? MEM32(req + 0x11C) : 0,
                a, MEM32(M + 0x17C), b, MEM32(M + 0x180));
        fflush(stderr);
    }
    {
        int a = strm_atomic();
        if (a) xbox_NoPreemptEnter();
        sub_00177990_gen();
        if (a) xbox_NoPreemptLeave();
    }
}

/*
 * EA STRM ring trace (always on, cheap: a host ring of the last 512 events,
 * printed with every thread dump). The title screen's background movie
 * freezes the frontend in ~1 run in 6 under load: the main thread loops in
 * sub_00177F80 (next chunk of a stream: walk the ring from the current chunk
 * to the next block tagged with the stream's id) and its walk pointer ends up
 * outside the ring -- the block chain is corrupt (the main-thread crash at
 * 0x00173B36 is the same walk hitting unmapped memory). Recorded: block scans
 * (sub_00177760, eax = manager), chunk fetches (sub_00177F80(h)), releases
 * (sub_00178540(h, chunk)), flushes (sub_00178800(h)), the read scheduler
 * (sub_00178280, K/k) and read completions (sub_001781B0, C/c), with thread, time
 * and the manager's ring pointers (+0x60 read, +0x64 scan, +0x68 write).
 */
typedef struct { DWORD ms, tid; char op; uint32_t m, a, b, c, d, e; } StrmEv;
#define STRM_RING 2048
static StrmEv s_strm_ev[STRM_RING];
static volatile LONG s_strm_n;
static volatile uint32_t s_strm_mgrs[8];

static void strm_note(char op, uint32_t m, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    LONG i = InterlockedIncrement(&s_strm_n) - 1;
    StrmEv *v = &s_strm_ev[i % STRM_RING];
    int k;
    v->ms = GetTickCount(); v->tid = GetCurrentThreadId(); v->op = op;
    v->m = m; v->a = a; v->b = b; v->c = c; v->d = d; v->e = e;
    for (k = 0; k < 8; k++) {
        if (s_strm_mgrs[k] == m) break;
        if (!s_strm_mgrs[k]) { s_strm_mgrs[k] = m; break; }
    }
}

static int strm_ok(uint32_t va) { return va >= 0x80000000u && va < 0x84000000u; }

extern void sub_00177760_gen(void);
extern void sub_00177F80_gen(void);
extern void sub_00178540_gen(void);
extern void sub_00178800_gen(void);



extern void sub_00178280_gen(void);
extern void sub_001781B0_gen(void);



void sub_00178540(void)                 /* release chunk */
{
    uint32_t h = MEM32(g_esp + 4), ch = MEM32(g_esp + 8), M = strm_ok(h) ? MEM32(h) : 0;
    strm_note('r', M, h, ch, M ? MEM32(M + 0x60) : 0, M ? MEM32(M + 0x64) : 0, M ? MEM32(M + 0x68) : 0);
    sub_00178540_gen();
    strm_note('R', M, h, ch, M ? MEM32(M + 0x60) : 0, M ? MEM32(M + 0x64) : 0, M ? MEM32(M + 0x68) : 0);
}

void sub_00178800(void)                 /* flush stream h */
{
    uint32_t h = MEM32(g_esp + 4), M = strm_ok(h) ? MEM32(h) : 0;
    strm_note('F', M, h, M ? MEM32(M + 0x60) : 0, M ? MEM32(M + 0x64) : 0, M ? MEM32(M + 0x68) : 0, 0);
    sub_00178800_gen();
    strm_note('f', M, h, M ? MEM32(M + 0x60) : 0, M ? MEM32(M + 0x64) : 0, M ? MEM32(M + 0x68) : 0, 0);
}

void ssx3_StrmTraceDump(void)
{
    LONG n = s_strm_n, i, first = n > STRM_RING ? n - STRM_RING : 0;
    int k;
    fprintf(stderr, "[STRMTRACE] last %ld of %ld (ms tid op M a b c d e)\n", n - first, n);
    for (i = first; i < n; i++) {
        const StrmEv *v = &s_strm_ev[i % STRM_RING];
        fprintf(stderr, "[STRMEV] %lu %lu %c %08X %08X %08X %08X %08X %08X\n", v->ms, v->tid, v->op,
                v->m, v->a, v->b, v->c, v->d, v->e);
    }
    for (k = 0; k < 8 && s_strm_mgrs[k]; k++) {
        uint32_t M = s_strm_mgrs[k], p, j;
        if (!strm_ok(M) || MEM32(M) != 0x4D525453u)
            continue;
        fprintf(stderr, "[STRMRING] M %08X ring %08X-%08X rd %08X scan %08X wr %08X busy %u | blocks from rd:",
                M, MEM32(M + 0x3C), MEM32(M + 0x44), MEM32(M + 0x60), MEM32(M + 0x64), MEM32(M + 0x68),
                MEM32(M + 0x48));
        for (p = MEM32(M + 0x60), j = 0; j < 40 && strm_ok(p); j++) {
            uint32_t t = MEM32(p), w = MEM32(p + 4);
            fprintf(stderr, " %08X:%08X/%08X", p, t, w);
            if (p == MEM32(M + 0x68)) break;
            if (t == 0xFFFFFFFFu) p = MEM32(M + 0x40);
            else if ((w & 0xFFFFFFu) == 0) break;
            else p += w & 0xFFFFFFu;
        }
        fprintf(stderr, "\n");
    }
    fflush(stderr);
}

static volatile uint32_t s_block_t;     /* BlockOnTime: time being waited for */
static volatile DWORD    s_block_since; /* when that call began, 0 = none */

/* Printed with every thread dump (src/thread_dump.c): the streamer's request
 * list (manager at streamer+0x10: [+0] streamer, [+4] world header with the
 * entry count at +8, [+8] entries of 0x58 bytes -- name at +0, slot at
 * +0x18 -- and per entry a state/param pair at +0x14 + 8*i), each busy
 * entry's slot record (streamer + 0x3EC + slot*24), and D3D's fence wait if
 * the main thread is in one. States: 0 idle, 1 requested, 2 loading,
 * 3 unloading, 4 off, 5 ready, 6 ready+in use. */
void ssx3_StreamDump(void)
{
    uint32_t S = s_strm_obj, mgr, hdr, ents, n, i, shown = 0;
    if (s_block_since) {
        uint32_t dev = MEM32(0x0024B0D8u);
        fprintf(stderr, "[STREAMDUMP] BlockOnTime waiting %lu ms for time %08X | fence %08X cur %08X"
                " wr %08X put %08X get %08X\n",
                (unsigned long)(GetTickCount() - s_block_since), s_block_t,
                dev ? MEM32(MEM32(dev + 0x30)) : 0, dev ? MEM32(dev + 0x2C) : 0,
                dev ? MEM32(dev) : 0, MEM32(0xFD800040u), MEM32(0xFD800044u));
    }
    if (!S || S < 0x10000u) {
        fprintf(stderr, "[STREAMDUMP] no streamer seen yet\n");
        fflush(stderr);
        return;
    }
    mgr = S + 0x10;
    hdr = MEM32(mgr + 4);
    ents = MEM32(mgr + 8);
    n = hdr ? MEM32(hdr + 8) : 0;
    fprintf(stderr, "[STREAMDUMP] streamer %08X mgr[0] %08X header %08X entries %08X count %u"
            " | +2E0 %u +2E4 %u | owner t%lu %s\n", S, MEM32(mgr), hdr, ents, n,
            MEM32(mgr + 0x2E0), MEM32(mgr + 0x2E4), (unsigned long)s_strm_owner,
            s_strm_owner ? s_strm_what : "-");
    if (n > 89)
        n = 89;                                   /* state pairs end at +0x2E0 */
    for (i = 0; i < n; i++) {
        uint32_t st = MEM32(mgr + 0x14 + 8 * i), par = MEM32(mgr + 0x18 + 8 * i);
        uint32_t e = ents + i * 0x58, slot = MEM32(e + 0x18);
        uint32_t so = MEM32(mgr) + 0x3EC + slot * 24;
        char name[5];
        if (st == 0 || st == 4)
            continue;
        for (int k = 0; k < 4; k++) {
            uint8_t c = MEM8(e + k);
            name[k] = c >= 32 && c <= 126 ? (char)c : '.';
        }
        name[4] = 0;
        fprintf(stderr, "[STREAMDUMP] #%u %s state %u param %08X slot %u -> %08X %08X %08X %08X %08X %08X\n",
                i, name, st, par, slot,
                slot < 4096 ? MEM32(so) : 0, slot < 4096 ? MEM32(so + 4) : 0,
                slot < 4096 ? MEM32(so + 8) : 0, slot < 4096 ? MEM32(so + 12) : 0,
                slot < 4096 ? MEM32(so + 16) : 0, slot < 4096 ? MEM32(so + 20) : 0);
        shown++;
    }
    {
        /* The reader (streamer+0x300) and EA's 'STRM' stream manager behind
         * it: handle [R+0x88] -> [h] = manager M (lock M+4, busy M+0x48,
         * request list from M+0x6C linked by +0xC, current M+0x70; each
         * request: +4 state, +0x10 kind, +0x14 file name, +0x118/+0x11C
         * offset/size). */
        uint32_t R = S + 0x300, h = MEM32(R + 0x88), M = 0, q, k;
        fprintf(stderr, "[STREAMDUMP] reader busy %u slot %d +8C %08X +9C %08X +AC %u +B0 %08X +B4 %08X"
                " +D0 %u | handle %08X", MEM8(R + 0x90), (int)MEM32(R + 0x94), MEM32(R + 0x8C),
                MEM32(R + 0x9C), MEM8(R + 0xAC), MEM32(R + 0xB0), MEM32(R + 0xB4), MEM8(R + 0xD0), h);
        if (h >= 0x10000u && h < 0x84000000u) {
            M = MEM32(h);
            fprintf(stderr, " -> M %08X id %u avail %d cur %08X", M, MEM32(h + 4),
                    (int)MEM32(h + 8), MEM32(h + 12));
        }
        fprintf(stderr, "\n");
        if (M >= 0x10000u && M < 0x84000000u && MEM32(M) == 0x4D525453u) {
            for (k = 0x3C; k < 0x80; k += 0x20)
                fprintf(stderr, "[STREAMDUMP] M+%02X %08X %08X %08X %08X %08X %08X %08X %08X\n", k,
                        MEM32(M + k), MEM32(M + k + 4), MEM32(M + k + 8), MEM32(M + k + 12),
                        MEM32(M + k + 16), MEM32(M + k + 20), MEM32(M + k + 24), MEM32(M + k + 28));
            fprintf(stderr, "[STREAMDUMP] M+17C %08X %08X %08X lock %08X %08X %08X %08X\n",
                    MEM32(M + 0x17C), MEM32(M + 0x180), MEM32(M + 0x184), MEM32(M + 4),
                    MEM32(M + 8), MEM32(M + 12), MEM32(M + 16));
            for (q = MEM32(M + 0x6C), k = 0; q >= 0x10000u && q < 0x84000000u && k < 24;
                 q = MEM32(q + 0xC), k++) {
                char nm[33];
                int c;
                for (c = 0; c < 32; c++) {
                    uint8_t b = MEM8(q + 0x14 + c);
                    if (!b) break;
                    nm[c] = b >= 32 && b <= 126 ? (char)b : '.';
                }
                nm[c] = 0;
                fprintf(stderr, "[STREAMDUMP] req %08X%s state %u kind %u +0 %08X +8 %08X off %08X size %08X"
                        " +120 %08X %s\n", q, q == MEM32(M + 0x70) ? "*" : " ", MEM32(q + 4),
                        MEM32(q + 0x10), MEM32(q), MEM32(q + 8), MEM32(q + 0x118), MEM32(q + 0x11C),
                        MEM32(q + 0x120), nm);
            }
        }
    }
    if (s_pool_obj) {
        uint32_t P = s_pool_obj;
        fprintf(stderr, "[STREAMDUMP] pool %08X lists (count/free x size): %u/%d x %X, %u/%d x %X, %u/%d x %X\n",
                P, MEM32(P + 4), (int)MEM32(P + 8), MEM32(P), MEM32(P + 0x18), (int)MEM32(P + 0x1C),
                MEM32(P + 0x14), MEM32(P + 0x2C), (int)MEM32(P + 0x30), MEM32(P + 0x28));
    }
    fprintf(stderr, "[STREAMDUMP] %u busy entries; resource states 0x312AC8:", shown);
    for (i = 0; i < 64; i++)
        fprintf(stderr, " %u", MEM32(0x00312AC8u + i * 16));
    fprintf(stderr, "\n");
    fflush(stderr);
}

/*
 * D3D BlockOnTime (sub_00234BE0(time, flags), stdcall-ish: args at esp+4/+8),
 * wrapped for RECOMP_BLOCK_TRACE=1: logs the time waited for against the GPU
 * fence, the device's current time, its write pointer and PUT/GET when the
 * same time is waited for repeatedly, so a hang that waits for a time which
 * never comes shows it (and normal running logs nothing). Device at [0x0024B0D8]:
 * +0 write ptr, +0x2C current time, +0x30 -> fence; PUT/GET at 0xFD800040.
 */
extern void sub_00234BE0_gen(void);

/*
 * Rescue for a lost fence wake (default on; RECOMP_BLOCK_RESCUE=0 off).
 * BlockOnTime's far path sleeps on the event at dev+0x1DBC with no timeout
 * until the GPU's NO_OPERATION(5) software method sets it, and returns after
 * one wake. In ~1 run in 10 the wake never comes: the main thread waited
 * 417 s for time 0x1ECF while the fence stood at 0x1ED3 -- already past it --
 * with the executor idle, and the race never loaded ("43%" with draws
 * stopped). A watchdog thread checks every millisecond: a wait older than
 * 2 ms (was 200 ms: each rescue was a visible hitch, and input pressed
 * during it could be lost; D3D's own wake for a reached fence often took
 * 25-45 ms -- the executor's idle wake waits 30 ms on a 16 ms clock) whose
 * time the fence has reached (D3D's own test: cur - time >= cur -
 * fence) gets the wake it was owed, set from the host, and is logged as
 * [BLOCK-RESCUE] with the state of the software-method chain so the cause
 * can still be found. A wait whose time is NOT reached after 2 s is logged
 * once ([BLOCK-STUCK]) and left alone.
 */
extern void xbox_Nv2aSwmDiag(char *buf, size_t n);
extern uint32_t xbox_TickCountMs(void);
extern int  xbox_KeSetEventFromHost(uint32_t guest_va);

static DWORD WINAPI block_rescue_thread(LPVOID unused)
{
    DWORD rescued_call = 0, reported_call = 0;
    unsigned n = 0;
    char diag[512];
    (void)unused;
    for (;;) {
        DWORD since = s_block_since, now;
        uint32_t t = s_block_t, dev, fence, cur;
        Sleep(1);
        now = xbox_TickCountMs();
        {
            /* RECOMP_GUESTCPU_STATS=1: once a second, the longest the guest
             * CPU stayed with one thread (and which, host tid), the most
             * threads waiting for it, and how many holds passed 10 ms --
             * sampled from here, a host thread, every millisecond. */
            static int on = -1;
            static unsigned long hmax, htid, long_holds, last_tid, sec;
            static unsigned long share_tid[16], share_ms[16];
            static int nshare;
            static long wmax;
            extern void xbox_GuestCpuInfo(unsigned long *, unsigned long *, long *);
            if (on < 0)
                on = getenv("RECOMP_GUESTCPU_STATS") != NULL;
            if (on) {
                unsigned long tid, held;
                long w;
                xbox_GuestCpuInfo(&tid, &held, &w);
                {
                    /* A hold past 25 ms: that thread's stack, once per hold,
                     * at most 20 in a run. */
                    static unsigned long caught_tid, caught_since, caught;
                    extern void ssx3_StackOfThread(unsigned long tid, const char *why);
                    if (held > 25 && caught < 20 && (tid != caught_tid || now - held != caught_since)) {
                        char why[48];
                        caught_tid = tid; caught_since = now - held; caught++;
                        snprintf(why, sizeof why, "held%lums", held);
                        ssx3_StackOfThread(tid, why);
                    }
                }
                {
                    /* Share of the guest CPU per holder: one sample = 1 ms. */
                    int k;
                    for (k = 0; k < nshare && share_tid[k] != tid; k++)
                        ;
                    if (k == nshare && nshare < 16) { share_tid[k] = tid; share_ms[k] = 0; nshare++; }
                    if (k < nshare) share_ms[k]++;
                }
                if (held > hmax) { hmax = held; htid = tid; }
                if (w > wmax) wmax = w;
                if (held == 10 || (held > 10 && tid != last_tid)) long_holds++;
                last_tid = tid;
                if (now - sec >= 1000) {
                    if (sec)
                    {
                        extern DWORD g_ssx3_main_tid;
                        int k, r;
                        fprintf(stderr, "[GUESTCPU-STATS] longest hold %lu ms (thread %lu), holds over 10 ms %lu,"
                                " waiters <= %ld | share:", hmax, htid, long_holds, wmax);
                        for (r = 0; r < 4; r++) {
                            int b = -1;
                            for (k = 0; k < nshare; k++)
                                if (share_ms[k] && (b < 0 || share_ms[k] > share_ms[b])) b = k;
                            if (b < 0) break;
                            fprintf(stderr, " %s%lu %lu ms", !share_tid[b] ? "free" : share_tid[b] == g_ssx3_main_tid ? "main:" : "",
                                    share_tid[b] ? share_tid[b] : 0ul, share_ms[b]);
                            share_ms[b] = 0;
                        }
                        fprintf(stderr, "\n");
                    }
                    sec = now; hmax = htid = long_holds = 0; wmax = 0; nshare = 0;
                }
            }
        }
        {
            /* RECOMP_SLOW_STACKS=<ms>: in game, a frame still open after
             * <ms> (no flip yet): the native stacks of the main thread, the
             * GPU executor and the flip thread, right then ([HOLDSTACK]
             * slowNNNms-main / -exec / -flip; tools/slowstacks.py names
             * them), and again every
             * further <ms> while it lasts. At most 60 in a run. */
            static int thr = -1;
            static uint32_t sampled_flip, next_at;
            static unsigned caught;
            extern volatile uint32_t ssx3_last_flip_ms, ssx3_exec_tid, ssx3_flip_tid;
            extern DWORD g_ssx3_main_tid;
            extern void ssx3_StackOfThread(unsigned long tid, const char *why);
            if (thr < 0) {
                const char *e = getenv("RECOMP_SLOW_STACKS");
                thr = e ? atoi(e) : 0;
                if (thr && thr < 20) thr = 50;
            }
            if (thr && caught < 60) {
                uint32_t lf = ssx3_last_flip_ms;
                if (lf != sampled_flip) { sampled_flip = lf; next_at = (uint32_t)thr; }
                /* (a flip right after `now` was read makes now - lf negative) */
                if (lf && (int32_t)(now - lf) >= (int32_t)next_at && ssx3_InGame()) {
                    char why[48];
                    snprintf(why, sizeof why, "slow%lums-main", (unsigned long)(now - lf));
                    if (g_ssx3_main_tid) ssx3_StackOfThread(g_ssx3_main_tid, why);
                    snprintf(why, sizeof why, "slow%lums-exec", (unsigned long)(now - lf));
                    if (ssx3_exec_tid) ssx3_StackOfThread(ssx3_exec_tid, why);
                    snprintf(why, sizeof why, "slow%lums-flip", (unsigned long)(now - lf));
                    if (ssx3_flip_tid) ssx3_StackOfThread(ssx3_flip_tid, why);
                    next_at += (uint32_t)thr;
                    caught++;
                }
            }
        }
        if (!since || since != s_block_since || now - since < 2)
            continue;
        dev = MEM32(0x0024B0D8u);
        if (!dev)
            continue;
        fence = MEM32(MEM32(dev + 0x30));
        cur = MEM32(dev + 0x2C);
        if (cur - t >= cur - fence) {
            if (since == rescued_call)
                continue;               /* already woken; let it run */
            rescued_call = since;
            n++;
            if (n <= 5 || n % 1000 == 0) {
                xbox_Nv2aSwmDiag(diag, sizeof diag);
                fprintf(stderr, "[BLOCK-RESCUE] #%u waited %lu ms for time %08X, fence %08X cur %08X"
                        " put %08X get %08X | %s\n", n, (unsigned long)(now - since), t, fence, cur,
                        MEM32(0xFD800040u), MEM32(0xFD800044u), diag);
                fflush(stderr);
            }
            xbox_KeSetEventFromHost(dev + 0x1DBC);
        } else if (now - since >= 2000 && since != reported_call) {
            reported_call = since;
            xbox_Nv2aSwmDiag(diag, sizeof diag);
            fprintf(stderr, "[BLOCK-STUCK] waited %lu ms for time %08X, fence %08X cur %08X wr %08X"
                    " put %08X get %08X | %s\n", (unsigned long)(now - since), t, fence, cur,
                    MEM32(dev), MEM32(0xFD800040u), MEM32(0xFD800044u), diag);
            fflush(stderr);
        }
    }
}

void sub_00234BE0(void)
{
    static volatile LONG rescue_started;
    static int on = -1;
    static uint32_t last_t;
    static unsigned repeats;
    uint32_t t = MEM32(g_esp + 4), flags = MEM32(g_esp + 8);
    if (on < 0)
        on = getenv("RECOMP_BLOCK_TRACE") != NULL;
    if (on) {
        /* Quiet while waits complete: only a time waited for again and again
         * (1st, 2nd, 10th, 100th, ... repeat) is logged, with the fence
         * record D3D keeps for it (dev+0x60 + ((t >> 1) & 63) * 8: time,
         * pushbuffer position) and the words at that position, where the
         * wake-up NOP gets patched in (+0x10..+0x1C). */
        uint32_t dev = MEM32(0x0024B0D8u);
        if (t != last_t) {
            repeats = 0;
            last_t = t;
        } else if (dev && (++repeats <= 2 || repeats == 10 || repeats == 100
                           || repeats % 1000 == 0)) {
            uint32_t rec = dev + 0x60 + ((t >> 1) & 63u) * 8, pos = MEM32(rec + 4);
            int pos_ok = pos >= 0x80000000u && pos < 0x84000000u;
            fprintf(stderr, "[BLOCK] %lu time %08X flags %X rep %u | fence %08X cur %08X | wr %08X"
                    " put %08X get %08X pb %08X-%08X ring %X | rec %08X @%08X: %08X %08X %08X %08X\n",
                    (unsigned long)GetTickCount(), t, flags, repeats,
                    MEM32(MEM32(dev + 0x30)), MEM32(dev + 0x2C), MEM32(dev),
                    MEM32(0xFD800040u), MEM32(0xFD800044u), MEM32(dev + 0x24), MEM32(dev + 0x28),
                    MEM32(dev + 0x40), MEM32(rec), pos,
                    pos_ok ? MEM32(pos + 0x10) : 0, pos_ok ? MEM32(pos + 0x14) : 0,
                    pos_ok ? MEM32(pos + 0x18) : 0, pos_ok ? MEM32(pos + 0x1C) : 0);
            fflush(stderr);
        }
    }
    if (!rescue_started && InterlockedExchange(&rescue_started, 1) == 0) {
        const char *e = getenv("RECOMP_BLOCK_RESCUE");
        if (!(e && e[0] == '0'))
            CloseHandle(CreateThread(NULL, 0, block_rescue_thread, NULL, 0, NULL));
    }
    s_block_t = t;
    s_block_since = xbox_TickCountMs();
    sub_00234BE0_gen();
    s_block_since = 0;
}

/*
 * D3D's PGRAPH interrupt handler (sub_0023BA20, called from its interrupt DPC
 * 0x0023BC60), wrapped so a software-method trap is seen exactly once. The
 * handler acknowledges by writing PGRAPH_INTR back with the value it read --
 * write-1-to-clear on hardware, a no-op on our RAM-backed registers -- and the
 * DPC re-runs it for as long as PMC_INTR_0 bit 12 stays set. Every re-run
 * handled the same trapped NOP again: SSX 3 schedules flips with them, and
 * one trap became thousands of flips, each pushing the flip target a vblank
 * further out until display changes waited forever. The toolkit clears the
 * trap the moment the handler returns.
 */
extern void sub_0023BA20_gen(void);
extern void xbox_Nv2aSwmHandled(void);

void sub_0023BA20(void)
{
    sub_0023BA20_gen();
    xbox_Nv2aSwmHandled();
}

/*
 * EA's allocator (sub_001A9BD0, ~300 direct callers), wrapped to report
 * failures. The race load (Snow Jam) ends up using a NULL script register
 * array allocated in sub_0016B4F0; this says whether the heap -- a fixed
 * 0x2C80000-byte XPhysicalAlloc block at [0x3A9334], size [0x3A9338], heap
 * object 0x3A9368 -- is really exhausted or a single request is bad.
 * cdecl: [esp+4] size, [esp+8] name/tag, [esp+12] flags.
 */
extern void sub_001A9BD0_gen(void);

void sub_001A9BD0(void)
{
    static volatile LONG failures, calls;
    static volatile long long requested;
    uint32_t ret_va = MEM32(g_esp);
    uint32_t size = MEM32(g_esp + 4);
    uint32_t tag = MEM32(g_esp + 8);
    uint32_t flags = MEM32(g_esp + 12);

    InterlockedIncrement(&calls);
    #ifdef _WIN32
    InterlockedExchangeAdd64(&requested, size);
#else
    __sync_fetch_and_add(&requested, size);
#endif
    sub_001A9BD0_gen();
    if (g_eax == 0 && InterlockedIncrement(&failures) <= 24)
        fprintf(stderr, "  [ALLOC] FAILED size=0x%X (%u) tag=0x%08X flags=0x%08X "
                        "caller=0x%08X  (call #%ld, %lld bytes requested so far)\n",
                size, size, tag, flags, ret_va, calls, requested);
}

/*
 * Bring-up tracing for the frame-timer object (0x80592400 on a normal boot):
 * the main thread waits on its +0x10 event in sub_001ADF40, the worker thread
 * (sub_001AE0E0) waits on +0x34, and sub_001AE020 is its timeSetEvent
 * callback. All are reached through vtables or the timer thread's callback
 * pointer, so recomp_lookup_manual routes them here without regenerating.
 * Enabled by RECOMP_TRACE_FRAMETIMER=1.
 */
#include <stdio.h>
#include <stdlib.h>

extern void sub_001AE020(void);
extern void sub_001ADF40(void);
extern void sub_001ADF50(void);
extern void sub_001ADF60(void);
extern void sub_001ADF70(void);
extern void sub_001ADF90(void);
extern void sub_000E1AE0(void);   /* app vtbl[6]: sample input into the ring */
extern void sub_000E1A80(void);   /* app vtbl[5]: is an input frame (update) due */
extern void sub_000D7460(void);   /* loading state vtbl[6]: render */

static void trace_call(const char *name, void (*fn)(void), volatile LONG *n)
{
    long k = InterlockedIncrement(n);
    uint32_t ecx = g_ecx;

    if (k <= 8 || (k & 63) == 0)
        fprintf(stderr, "  [FRAMETIMER] %s #%ld tid=%lu ecx=%08X t=%lu\n",
                name, k, GetCurrentThreadId(), ecx, GetTickCount());
    fn();
}

/* Same, but logs the return value (al) whenever it changes, with the input
 * ring indices: guest 0x0036952C is the input manager, +0x2EE0 read and
 * +0x2EE4 write. */
static void trace_ret(const char *name, void (*fn)(void), volatile LONG *n,
                      volatile LONG *last)
{
    long k = InterlockedIncrement(n);
    long ret;
    uint32_t im, edi_in = g_edi, esi_in = g_esi, ebx_in = g_ebx;

    fn();
    ret = (long)(g_eax & 0xFF);
    if (ret != *last || (k & 63) == 0) {
        im = MEM32(0x0036952C);
        fprintf(stderr, "  [GATE] %s #%ld -> %ld  t=%lu  ring r=%u w=%u"
                "  edi_in=%08X esi_in=%08X ebx_in=%08X\n",
                name, k, ret, GetTickCount(),
                im ? MEM32(im + 0x2EE0) : 0, im ? MEM32(im + 0x2EE4) : 0,
                edi_in, esi_in, ebx_in);
        *last = ret;
    }
}

#define TRACED_RET(va)                                                 \
    static volatile LONG n_##va, last_##va = -1;                       \
    static void traced_##va(void) { trace_ret(#va, va, &n_##va, &last_##va); }

#define TRACED(va)                                                     \
    static volatile LONG n_##va;                                       \
    static void traced_##va(void) { trace_call(#va, va, &n_##va); }

TRACED(sub_001AE020)
TRACED(sub_001ADF40)
TRACED(sub_001ADF50)
TRACED(sub_001ADF60)
TRACED(sub_001ADF70)
TRACED(sub_001ADF90)
TRACED(sub_000E1AE0)
TRACED_RET(sub_000E1A80)
TRACED_RET(sub_000D7460)

/* RECOMP_FRAMETIMER_STATS=1: how regularly the frame timer's callback
 * (sub_001AE020, a one-shot XAPI timer the game re-arms every 1000/60 ms
 * from its own millisecond clock) actually runs -- per second: calls, mean,
 * min and max interval, and how many were over 20 ms. */
static void frametimer_stats(void)
{
    static LARGE_INTEGER f, last, sec;
    static double sum, mn = 1e9, mx, gap_max, fire_late_max;
    static unsigned long hold_max, hold_tid;
    static long wait_max;
    static int n, late;
    LARGE_INTEGER now;
    extern double xbox_TimerLastFire(double *late_ms, double *now_ms);
    extern void xbox_GuestCpuInfo(unsigned long *tid, unsigned long *held_ms, long *waiters);
    {
        /* Who holds the guest CPU right now, and for how long: the timer
         * work (DPCs) needs it, and late in a long session it waited for it. */
        unsigned long tid, held;
        long waiters;
        xbox_GuestCpuInfo(&tid, &held, &waiters);
        if (held > hold_max) { hold_max = held; hold_tid = tid; }
        if (waiters > wait_max) wait_max = waiters;
    }
    {
        /* The last kernel timer to fire is (nearly always) the XAPI timer
         * queue's: the gap to here is its thread getting going. */
        double fl, qn, fired = xbox_TimerLastFire(&fl, &qn);
        if (qn - fired > gap_max) gap_max = qn - fired;
        if (fl > fire_late_max) fire_late_max = fl;
    }
    QueryPerformanceCounter(&now);
    if (!f.QuadPart) {
        QueryPerformanceFrequency(&f);
        sec = now;
    }
    if (last.QuadPart) {
        double d = (double)(now.QuadPart - last.QuadPart) * 1e3 / (double)f.QuadPart;
        sum += d; n++;
        if (d < mn) mn = d;
        if (d > mx) mx = d;
        if (d > 20.0) late++;
    }
    last = now;
    if ((double)(now.QuadPart - sec.QuadPart) >= (double)f.QuadPart) {
        if (n)
            fprintf(stderr, "[FRAMETIMER] %d calls, interval mean %.2f min %.2f max %.2f ms, %d over 20 ms"
                    " | timer fired late <= %.2f ms, fire->callback <= %.2f ms"
                    " | guest CPU held <= %lu ms (thread %lu), waiters <= %ld\n",
                    n, sum / n, mn, mx, late, fire_late_max, gap_max, hold_max, hold_tid, wait_max);
        sec = now; sum = 0; n = late = 0; mn = 1e9; mx = 0; gap_max = fire_late_max = 0;
        hold_max = hold_tid = 0; wait_max = 0;
    }
    sub_001AE020();
}

void *ssx3_lookup_trace(uint32_t va)
{
    static int on = -1, stats = -1;

    if (stats < 0)
        stats = getenv("RECOMP_FRAMETIMER_STATS") != NULL;
    if (stats && va == 0x001AE020)
        return frametimer_stats;

    if (on < 0)
        on = getenv("RECOMP_TRACE_FRAMETIMER") != NULL;
    if (!on)
        return NULL;
    switch (va) {
    case 0x001AE020: return traced_sub_001AE020;
    case 0x001ADF40: return traced_sub_001ADF40;
    case 0x001ADF50: return traced_sub_001ADF50;
    case 0x001ADF60: return traced_sub_001ADF60;
    case 0x001ADF70: return traced_sub_001ADF70;
    case 0x001ADF90: return traced_sub_001ADF90;
    case 0x000E1AE0: return traced_sub_000E1AE0;
    case 0x000E1A80: return traced_sub_000E1A80;
    case 0x000D7460: return traced_sub_000D7460;
    }
    return NULL;
}
