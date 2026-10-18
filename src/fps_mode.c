/*
 * First-person shooter mode (RECOMP_FPS=1; F4 toggles it in a race). Just
 * for fun: the race camera sits in the player's head, the mouse aims, the
 * left button fires, and a rider that is hit wipes out.
 *
 * What it uses of the game (found 2026-10-17, all in the in-game state):
 *   - rider manager M = [[[0x369534]+0x74]+0xC] (vtable 0x0028216C, built
 *     by sub_000139C0 / sub_00013C60): rider pointers at M+0x28 + 4*i, up to
 *     6; slot 0 is player 1. The pointer is the rider base the physics code
 *     works on: position +0x180 (x, y, z -- the world is Z-up), velocity
 *     +0x250, physics object [+0x74C] whose mode +0xE60 names a state from
 *     the table at 0x00283274 (0 Ground, 1 Air, 2 Wipeout, ...).
 *   - wipeout: sub_0002F620 (thiscall on the rider, ret 0x10) with (anim,
 *     0, [rider+0x498], impact*) -- the call the balance meter makes when a
 *     rider loses it (sub_000373F0: anim 0x166/0x167). impact: +0x00
 *     position, +0x10 direction, +0x20 normal, +0x30 speed.
 *   - camera: player 1's view object [[ingame state+0x84]+4] (vtable
 *     0x0028630C), finished each update by sub_0005C000: +0x10 aspect,
 *     +0x14 near, +0x18 far, +0x1C fov, +0x30 eye, +0x40 orientation
 *     quaternion (its matrix has the rows forward, -right, up), +0x50 the
 *     D3D view matrix (rows (r.x u.x f.x 0) .. and -eye.(r u f) 1), +0x90
 *     forward. The hook overwrites these after the game's update, before
 *     the frame is rendered.
 *
 * Mouse: Windows recentres the cursor in the game window each update;
 * Linux uses SDL's relative mouse mode (when the SDL window is in use).
 * Each shot kicks the view up; the gun (a small 3D model rendered once
 * per frame size into cached sprites), its recoil, slide, muzzle flash,
 * brass and smoke, the hit mist, crosshair and hit counter are drawn into
 * each flipped frame (vkr_backend.c flip thread). The shot sound is
 * synthesised here and mixed into the APU output through the toolkit's
 * xbox_ApuPostMix hook (patches/xboxrecomp-local.patch).
 *
 *   RECOMP_FPS=1              start with it on (F4 toggles)
 *   RECOMP_FPS_SENS=<f>       mouse sensitivity (default 1)
 *   RECOMP_FPS_EYE=<units>    eye height above the rider (default 150)
 *   RECOMP_FPS_AUTOFIRE=<ms>  test aid: fire on its own every <ms>, aimed
 *                             at the nearest rider in front
 *   RECOMP_FPS_TRACE=1        log the player's physics mode changes
 *   RECOMP_FPS_SHOTS=<dir>    test aid: save the first frames with a muzzle
 *                             flash as <dir>/flash<n>.ppm
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

#define APP_PTR_VA   0x003A930Cu
#define VT_INGAME    0x00299D7Cu
#define VT_RIDERMAN  0x0028216Cu
#define VT_CAMERA    0x0028630Cu
#define MODE_WIPEOUT 2
#define MAX_RIDERS   6

extern uint32_t xbox_TickCountMs(void);
extern void sub_0002F620(void);   /* the rider wipeout */

static int s_inited, s_enabled, s_autofire_ms;
static float s_sens = 1.0f, s_eye_h = 150.0f;
static float s_yaw, s_pitch;          /* view: heading from the velocity + mouse */
static float s_heading;              /* smoothed travel heading (radians) */
static int s_have_heading, s_was_active, s_fire_held, s_trace, s_last_mode = -1;
static uint32_t s_live_rider;        /* player rider whose race has started */
static float s_kick_p, s_kick_y;     /* recoil on the view (radians) */
static float s_sway_xs, s_sway_ys, s_bob_ph, s_lean, s_prev_heading;
static uint32_t s_last_tick;
static float hash01(uint32_t x);
static uint32_t s_next_shot, s_next_auto;

/* Read by the flip thread for the overlay. */
static volatile int s_active;        /* in a race with the mode on */
static volatile int s_hits;
/* The shots (ring of 16), for the overlay and the sound; written by the tick. */
typedef struct { uint32_t t, seed; int hit; float hp[3]; } Shot;
static Shot s_shots[16];
static volatile uint32_t s_nshots;
static volatile uint32_t s_snd_req;  /* shots the audio thread should play */
/* gun motion (fractions of the frame height / radians) and the camera */
static volatile float s_sway_x, s_sway_y, s_roll, s_bob;
static volatile float s_cam_eye[3], s_cam_r[3], s_cam_u[3], s_cam_f[3];
static void snd_init(void);

static int guest_ok(uint32_t va)
{
    return va >= 0x10000u && (va < 0x04000000u || (va >= 0x80000000u && va < 0x84000000u));
}

static float rdf(uint32_t va) { return MEMF(va); }
static void wrf(uint32_t va, float v) { MEMF(va) = v; }

/* ---------------------------------------------------------------- input */

#ifdef _WIN32
static HWND s_hwnd;
static HCURSOR s_old_cursor;
static int s_cursor_hidden, s_skip_delta;

static BOOL CALLBACK find_window(HWND h, LPARAM l)
{
    DWORD pid = 0;
    (void)l;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(h)) {
        s_hwnd = h;
        return FALSE;
    }
    return TRUE;
}

static int window_focused(void)
{
    if (!s_hwnd || !IsWindow(s_hwnd)) {
        s_hwnd = NULL;
        EnumWindows(find_window, 0);
    }
    return s_hwnd && GetForegroundWindow() == s_hwnd;
}

static void hide_cursor(int hide)
{
    if (!s_hwnd || hide == s_cursor_hidden)
        return;
    if (hide) {
        s_old_cursor = (HCURSOR)GetClassLongPtrA(s_hwnd, GCLP_HCURSOR);
        SetClassLongPtrA(s_hwnd, GCLP_HCURSOR, 0);
        SetCursor(NULL);
        s_skip_delta = 1;
    } else {
        SetClassLongPtrA(s_hwnd, GCLP_HCURSOR, (LONG_PTR)s_old_cursor);
        SetCursor(s_old_cursor ? s_old_cursor : LoadCursor(NULL, IDC_ARROW));
    }
    s_cursor_hidden = hide;
}

static int key_pressed(int vk)
{
    return window_focused() && (GetAsyncKeyState(vk) & 0x8000);
}

/* Mouse movement since the last call (pixels) and the left button. */
static void mouse_poll(int active, int *dx, int *dy, int *fire)
{
    RECT rc;
    POINT c, p;
    *dx = *dy = *fire = 0;
    if (!active || !window_focused()) {
        hide_cursor(0);
        s_skip_delta = 1;
        return;
    }
    hide_cursor(1);
    GetClientRect(s_hwnd, &rc);
    c.x = (rc.right - rc.left) / 2;
    c.y = (rc.bottom - rc.top) / 2;
    ClientToScreen(s_hwnd, &c);
    GetCursorPos(&p);
    if (!s_skip_delta) {
        *dx = p.x - c.x;
        *dy = p.y - c.y;
    }
    s_skip_delta = 0;
    SetCursorPos(c.x, c.y);
    *fire = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
}
#else
/* SDL (the POSIX window): weak, so a build without SDL still links. */
extern int SDL_SetRelativeMouseMode(int enabled) __attribute__((weak));
extern uint32_t SDL_GetRelativeMouseState(int *x, int *y) __attribute__((weak));
extern const uint8_t *SDL_GetKeyboardState(int *numkeys) __attribute__((weak));
static int s_rel_mode;

static int key_pressed(int vk)
{
    const uint8_t *k;
    int sc;
    if (vk >= 'A' && vk <= 'Z')
        sc = 4 + (vk - 'A');                    /* SDL_SCANCODE_A.. */
    else if (vk >= 0x70 && vk <= 0x7B)
        sc = 58 + (vk - 0x70);                  /* VK_F1.. -> SDL_SCANCODE_F1.. */
    else if (vk >= '1' && vk <= '9')
        sc = 30 + (vk - '1');                   /* SDL_SCANCODE_1.. */
    else if (vk == '0')
        sc = 39;
    else if (vk == 0x20)
        sc = 44;                                /* SDL_SCANCODE_SPACE */
    else
        return 0;
    if (!SDL_GetKeyboardState)
        return 0;
    k = SDL_GetKeyboardState(NULL);
    return k && k[sc];
}

static void mouse_poll(int active, int *dx, int *dy, int *fire)
{
    uint32_t b;
    *dx = *dy = *fire = 0;
    if (!SDL_SetRelativeMouseMode || !SDL_GetRelativeMouseState)
        return;
    if (active != s_rel_mode) {
        SDL_SetRelativeMouseMode(active);
        SDL_GetRelativeMouseState(NULL, NULL);
        s_rel_mode = active;
    }
    if (!active)
        return;
    b = SDL_GetRelativeMouseState(dx, dy);
    *fire = (b & 1u) != 0;                      /* SDL_BUTTON_LMASK */
}
#endif

/* ----------------------------------------------------------- the riders */

static uint32_t rider_manager(void)
{
    uint32_t g = MEM32(0x00369534u), a, m;
    if (!guest_ok(g)) return 0;
    a = MEM32(g + 0x74);
    if (!guest_ok(a)) return 0;
    m = MEM32(a + 0xC);
    return guest_ok(m) && MEM32(m) == VT_RIDERMAN ? m : 0;
}

static uint32_t rider(uint32_t m, int i)
{
    uint32_t b = MEM32(m + 0x28 + 4u * (uint32_t)i);
    return guest_ok(b) && guest_ok(MEM32(b + 0x74C)) ? b : 0;
}

static int rider_mode(uint32_t b)
{
    return (int)MEM32(MEM32(b + 0x74C) + 0xE60);
}

/* Knock rider b down: the game's own wipeout, with the bullet's direction. */
static void rider_wipeout(uint32_t b, const float dir[3])
{
    uint32_t sv_eax = g_eax, sv_ebx = g_ebx, sv_ecx = g_ecx, sv_edx = g_edx;
    uint32_t sv_esi = g_esi, sv_edi = g_edi, sv_ebp = g_ebp, sv_esp = g_esp;
    uint32_t sp = (g_esp - 0x200u) & ~0xFu, info = sp;
    float vx = rdf(b + 0x250), vy = rdf(b + 0x254), vz = rdf(b + 0x258);
    float speed = sqrtf(vx * vx + vy * vy + vz * vz);
    int k;

    for (k = 0; k < 4; k++)
        MEM32(info + 4u * k) = MEM32(b + 0x180 + 4u * k);   /* position */
    wrf(info + 0x10, dir[0]); wrf(info + 0x14, dir[1]);
    wrf(info + 0x18, dir[2]); wrf(info + 0x1C, 0.0f);
    wrf(info + 0x20, 0.0f); wrf(info + 0x24, 0.0f);          /* normal: down, */
    wrf(info + 0x28, -1.0f); wrf(info + 0x2C, 0.0f);          /* as sub_000373F0 */
    wrf(info + 0x30, speed > 1500.0f ? speed : 1500.0f);

    g_esp = sp;
    PUSH32(g_esp, info);
    PUSH32(g_esp, MEM32(b + 0x498));
    PUSH32(g_esp, 0);
    PUSH32(g_esp, 0x166);
    PUSH32(g_esp, 0);                           /* return address */
    g_ecx = b;
    sub_0002F620();

    g_eax = sv_eax; g_ebx = sv_ebx; g_ecx = sv_ecx; g_edx = sv_edx;
    g_esi = sv_esi; g_edi = sv_edi; g_ebp = sv_ebp; g_esp = sv_esp;
}

/* The rider nearest along the ray (eye, dir) within a body-sized sphere. */
static uint32_t ray_pick(uint32_t m, const float eye[3], const float dir[3], float radius, float *t_hit)
{
    uint32_t best = 0;
    float best_t = 1e30f;
    int i;
    for (i = 1; i < MAX_RIDERS; i++) {
        uint32_t b = rider(m, i);
        float c[3], t, d2;
        if (!b || rider_mode(b) == MODE_WIPEOUT)
            continue;
        c[0] = rdf(b + 0x180) - eye[0];
        c[1] = rdf(b + 0x184) - eye[1];
        c[2] = rdf(b + 0x188) + 90.0f - eye[2];   /* the body, not the board */
        t = c[0] * dir[0] + c[1] * dir[1] + c[2] * dir[2];
        if (t <= 0.0f || t > 60000.0f)
            continue;
        d2 = c[0] * c[0] + c[1] * c[1] + c[2] * c[2] - t * t;
        if (d2 < radius * radius && t < best_t) {
            best_t = t - sqrtf(radius * radius - d2) * 0.5f;   /* on the body's near side */
            best = b;
        }
    }
    *t_hit = best_t;
    return best;
}

/* ---------------------------------------------------------- the camera */

static uint32_t player_camera(uint32_t state)
{
    uint32_t pm = MEM32(state + 0x84), cam;
    if (!guest_ok(pm) || MEM32(pm + 0x10) < 1)
        return 0;
    cam = MEM32(pm + 4);
    return guest_ok(cam) && MEM32(cam) == VT_CAMERA ? cam : 0;
}

/* Quaternion of the rotation whose matrix has rows a, b, c. */
static void mat_to_quat(const float a[3], const float b[3], const float c[3], float q[4])
{
    float tr = a[0] + b[1] + c[2], s;
    if (tr > 0.0f) {
        s = sqrtf(tr + 1.0f) * 2.0f;
        q[3] = 0.25f * s;
        q[0] = (c[1] - b[2]) / s;
        q[1] = (a[2] - c[0]) / s;
        q[2] = (b[0] - a[1]) / s;
    } else if (a[0] > b[1] && a[0] > c[2]) {
        s = sqrtf(1.0f + a[0] - b[1] - c[2]) * 2.0f;
        q[3] = (c[1] - b[2]) / s;
        q[0] = 0.25f * s;
        q[1] = (a[1] + b[0]) / s;
        q[2] = (a[2] + c[0]) / s;
    } else if (b[1] > c[2]) {
        s = sqrtf(1.0f + b[1] - a[0] - c[2]) * 2.0f;
        q[3] = (a[2] - c[0]) / s;
        q[0] = (a[1] + b[0]) / s;
        q[1] = 0.25f * s;
        q[2] = (b[2] + c[1]) / s;
    } else {
        s = sqrtf(1.0f + c[2] - a[0] - b[1]) * 2.0f;
        q[3] = (b[0] - a[1]) / s;
        q[0] = (a[2] + c[0]) / s;
        q[1] = (b[2] + c[1]) / s;
        q[2] = 0.25f * s;
    }
}

static void write_camera(uint32_t cam, const float eye[3], const float f[3])
{
    float r[3], u[3], nr[3], q[4], len;
    int i;
    /* right = f x Z, up = right x f (the game's own camera does the same) */
    r[0] = f[1]; r[1] = -f[0]; r[2] = 0.0f;
    len = sqrtf(r[0] * r[0] + r[1] * r[1]);
    if (len < 1e-4f) { r[0] = 1.0f; r[1] = 0.0f; len = 1.0f; }
    r[0] /= len; r[1] /= len;
    u[0] = r[1] * f[2] - r[2] * f[1];
    u[1] = r[2] * f[0] - r[0] * f[2];
    u[2] = r[0] * f[1] - r[1] * f[0];

    for (i = 0; i < 3; i++) wrf(cam + 0x30 + 4u * i, eye[i]);
    wrf(cam + 0x3C, 1.0f);
    nr[0] = -r[0]; nr[1] = -r[1]; nr[2] = -r[2];
    mat_to_quat(f, nr, u, q);
    for (i = 0; i < 4; i++) wrf(cam + 0x40 + 4u * i, q[i]);
    for (i = 0; i < 3; i++) {
        wrf(cam + 0x50 + 0x10u * i + 0, r[i]);
        wrf(cam + 0x50 + 0x10u * i + 4, u[i]);
        wrf(cam + 0x50 + 0x10u * i + 8, f[i]);
        wrf(cam + 0x50 + 0x10u * i + 12, 0.0f);
    }
    wrf(cam + 0x80, -(r[0] * eye[0] + r[1] * eye[1] + r[2] * eye[2]));
    wrf(cam + 0x84, -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]));
    wrf(cam + 0x88, -(f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2]));
    wrf(cam + 0x8C, 1.0f);
    for (i = 0; i < 3; i++) wrf(cam + 0x90 + 4u * i, f[i]);
    wrf(cam + 0x9C, 0.0f);
}

/* ---------------------------------------- shared with gears_mode.c */

static int ingame_state(uint32_t *state);

int ssx3_HostKey(int vk) { return key_pressed(vk); }

/* Player 1's rider base while in the in-game state (0 if none); *mode its
 * physics mode (names at 0x00283274). */
uint32_t ssx3_PlayerRider(int *mode)
{
    uint32_t state, m, me;
    if (!ingame_state(&state) || !(m = rider_manager()) || !(me = rider(m, 0)))
        return 0;
    *mode = rider_mode(me);
    return me;
}

void ssx3_RiderWipeout(uint32_t b, const float dir[3]) { rider_wipeout(b, dir); }

/* Rider i (0 = player 1, up to 5) while in the in-game state, with its mode. */
uint32_t ssx3_RiderN(int i, int *mode)
{
    uint32_t state, m, b;
    if (!ingame_state(&state) || !(m = rider_manager()) || !(b = rider(m, i)))
        return 0;
    *mode = rider_mode(b);
    return b;
}

/* ------------------------------------------------------------ per update */

static void fps_init(void)
{
    const char *e;
    s_inited = 1;
    e = getenv("RECOMP_FPS");
    s_enabled = e && *e && *e != '0';
    if ((e = getenv("RECOMP_FPS_SENS")) && atof(e) > 0) s_sens = (float)atof(e);
    if ((e = getenv("RECOMP_FPS_EYE"))) s_eye_h = (float)atof(e);
    if ((e = getenv("RECOMP_FPS_AUTOFIRE"))) s_autofire_ms = atoi(e);
    s_trace = getenv("RECOMP_FPS_TRACE") != NULL;
    snd_init();
    if (s_enabled)
        fprintf(stderr, "[FPS] first-person shooter mode on (F4 toggles)\n");
}

static int ingame_state(uint32_t *state)
{
    uint32_t app = MEM32(APP_PTR_VA), cur;
    if (!guest_ok(app)) return 0;
    cur = MEM32(app + 4);
    if (!guest_ok(cur) || MEM32(cur) != VT_INGAME) return 0;
    *state = cur;
    return 1;
}

/* Called on the game thread after every in-game update (sub_000CC8F0). */
void ssx3_FpsTick(void)
{
    static int f4_was;
    uint32_t state, m, me, cam, now = xbox_TickCountMs();
    float pos[3], vel[3], eye[3], f[3], speed, cp;
    int dx, dy, fire, f4, i;

    if (!s_inited)
        fps_init();
    f4 = key_pressed(0x73);
    if (f4 && !f4_was) {
        s_enabled = !s_enabled;
        fprintf(stderr, "[FPS] %s\n", s_enabled ? "on" : "off");
    }
    f4_was = f4;

    m = 0; me = 0; cam = 0;
    if (s_enabled && ingame_state(&state) && (m = rider_manager()) != 0
            && (me = rider(m, 0)) != 0)
        cam = player_camera(state);
    /* Only once the race is on: the course fly-over and the start gate
     * keep the game's own cameras. The player's mode is Static (3) or
     * Gate (12) until the start; back at the gate (a restart) it waits
     * again. */
    if (cam) {
        int mode = rider_mode(me);
        if (s_trace && mode != s_last_mode)
            fprintf(stderr, "[FPS] t=%u player mode %d\n", now, mode);
        s_last_mode = mode;
        if (mode == 12)
            s_live_rider = 0;
        else if (mode != 3)
            s_live_rider = me;
        if (s_live_rider != me)
            cam = 0;
    } else {
        s_live_rider = 0;
        s_last_mode = -1;
    }
    s_active = cam != 0;
    mouse_poll(s_active, &dx, &dy, &fire);
    if (!s_active) {
        s_was_active = 0;
        return;
    }

    pos[0] = rdf(me + 0x180); pos[1] = rdf(me + 0x184); pos[2] = rdf(me + 0x188);
    vel[0] = rdf(me + 0x250); vel[1] = rdf(me + 0x254); vel[2] = rdf(me + 0x258);
    if (!isfinite(pos[0]) || !isfinite(pos[1]) || !isfinite(pos[2]))
        return;

    /* Heading: where the rider travels, smoothed; kept while (nearly) still. */
    speed = sqrtf(vel[0] * vel[0] + vel[1] * vel[1]);
    if (speed > 150.0f && isfinite(speed)) {
        float h = atan2f(vel[1], vel[0]), d;
        if (!s_have_heading) {
            s_heading = h;
            s_have_heading = 1;
        }
        d = h - s_heading;
        while (d > 3.14159265f) d -= 6.2831853f;
        while (d < -3.14159265f) d += 6.2831853f;
        s_heading += d * 0.15f;
    }
    if (!s_was_active) {
        s_yaw = 0.0f;
        s_pitch = -0.15f;
        s_was_active = 1;
    }
    s_yaw -= (float)dx * 0.0025f * s_sens;
    s_pitch -= (float)dy * 0.0025f * s_sens;
    if (s_pitch > 1.4f) s_pitch = 1.4f;
    if (s_pitch < -1.4f) s_pitch = -1.4f;
    while (s_yaw > 3.14159265f) s_yaw -= 6.2831853f;
    while (s_yaw < -3.14159265f) s_yaw += 6.2831853f;

    /* Recoil: each shot throws the view up (and a little sideways); it
     * settles back over ~0.2 s. */
    {
        float dt = s_last_tick && now > s_last_tick ? (float)(now - s_last_tick) / 1000.0f : 0.016f;
        float k = expf(-dt / 0.09f);
        if (dt > 0.1f) dt = 0.1f;
        s_kick_p *= k;
        s_kick_y *= k;
        /* gun sway: lags behind the mouse, bobs with the board, leans into turns */
        s_sway_xs += ((float)dx * 0.00025f - s_sway_xs) * 0.25f;
        s_sway_ys += ((float)dy * 0.00025f - s_sway_ys) * 0.25f;
        if (s_sway_xs > 0.05f) s_sway_xs = 0.05f;
        if (s_sway_xs < -0.05f) s_sway_xs = -0.05f;
        if (s_sway_ys > 0.05f) s_sway_ys = 0.05f;
        if (s_sway_ys < -0.05f) s_sway_ys = -0.05f;
        {
            float sp = sqrtf(vel[0] * vel[0] + vel[1] * vel[1] + vel[2] * vel[2]);
            float amp = sp > 2500.0f ? 1.0f : sp / 2500.0f;
            int air = rider_mode(me) != 0;
            s_bob_ph += dt * (6.0f + 5.0f * amp);
            s_bob = air ? -0.01f : (sinf(s_bob_ph) * 0.004f + sinf(s_bob_ph * 2.7f) * 0.0015f) * amp
                    + sinf((float)now * 0.0015f) * 0.002f;
        }
        {
            float dh = s_heading - s_prev_heading;
            while (dh > 3.14159265f) dh -= 6.2831853f;
            while (dh < -3.14159265f) dh += 6.2831853f;
            s_lean += (-dh / (dt > 0.001f ? dt : 0.016f) * 0.06f - s_lean) * 0.1f;
            if (s_lean > 0.12f) s_lean = 0.12f;
            if (s_lean < -0.12f) s_lean = -0.12f;
            s_prev_heading = s_heading;
        }
        s_sway_x = -s_sway_xs;
        s_sway_y = -s_sway_ys;
        s_roll = s_lean + s_sway_xs * 0.8f;
        s_last_tick = now;
    }

    cp = cosf(s_pitch + s_kick_p);
    f[0] = cosf(s_heading + s_yaw + s_kick_y) * cp;
    f[1] = sinf(s_heading + s_yaw + s_kick_y) * cp;
    f[2] = sinf(s_pitch + s_kick_p);
    eye[0] = pos[0] + f[0] * 25.0f;
    eye[1] = pos[1] + f[1] * 25.0f;
    eye[2] = pos[2] + s_eye_h;

    /* Test aid: aim at the nearest rider in front and fire. */
    if (s_autofire_ms > 0 && now >= s_next_auto) {
        float best = 1e30f, tf[3] = {0, 0, 0};
        int i, got = 0;
        for (i = 1; i < MAX_RIDERS; i++) {
            uint32_t b = rider(m, i);
            float c[3], d;
            if (!b || rider_mode(b) == MODE_WIPEOUT) continue;
            c[0] = rdf(b + 0x180) - eye[0];
            c[1] = rdf(b + 0x184) - eye[1];
            c[2] = rdf(b + 0x188) + 90.0f - eye[2];
            d = sqrtf(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
            if (d > 1.0f && d < best && c[0] * f[0] + c[1] * f[1] > 0.0f) {
                best = d; got = 1;
                tf[0] = c[0] / d; tf[1] = c[1] / d; tf[2] = c[2] / d;
            }
        }
        if (got) {
            s_yaw = atan2f(tf[1], tf[0]) - s_heading - s_kick_y;
            s_pitch = asinf(tf[2]) - s_kick_p;
            memcpy(f, tf, sizeof f);
            fire = 1;
        }
        s_next_auto = now + (uint32_t)s_autofire_ms;
    }

    /* Fire: one shot per click, every 150 ms while held. The bullet goes
     * where the crosshair was; then the view kicks. */
    if (fire && (!s_fire_held || now >= s_next_shot)) {
        float t_hit = 0;
        uint32_t hit = ray_pick(m, eye, f, 110.0f, &t_hit);
        uint32_t n = s_nshots;
        Shot *sh = &s_shots[n & 15];
        sh->t = now;
        sh->seed = (n + 1) * 2654435761u ^ now;
        sh->hit = hit != 0;
        sh->hp[0] = eye[0] + f[0] * t_hit;
        sh->hp[1] = eye[1] + f[1] * t_hit;
        sh->hp[2] = eye[2] + f[2] * t_hit;
        s_nshots = n + 1;
        s_snd_req++;
        s_next_shot = now + 150;
        s_kick_p += 0.045f + 0.02f * hash01(sh->seed + 1u);
        s_kick_y += (hash01(sh->seed + 2u) - 0.5f) * 0.025f;
        if (hit) {
            int i, slot = -1;
            for (i = 1; i < MAX_RIDERS; i++)
                if (rider(m, i) == hit) slot = i;
            rider_wipeout(hit, f);
            s_hits++;
            fprintf(stderr, "[FPS] hit rider %d (0x%08X), mode now %d\n",
                    slot, hit, rider_mode(hit));
        }
    }

    write_camera(cam, eye, f);
    for (i = 0; i < 3; i++) {
        s_cam_eye[i] = eye[i];
        s_cam_f[i] = f[i];
    }
    {
        float r0 = f[1], r1 = -f[0], len = sqrtf(r0 * r0 + r1 * r1);
        if (len < 1e-4f) { r0 = 1; r1 = 0; len = 1; }
        r0 /= len; r1 /= len;
        s_cam_r[0] = r0; s_cam_r[1] = r1; s_cam_r[2] = 0;
        s_cam_u[0] = -f[2] * r1;                 /* r x f */
        s_cam_u[1] = f[2] * r0;
        s_cam_u[2] = r0 * f[1] - r1 * f[0];
    }
    s_fire_held = fire;
}


/* =================================================================== *
 * The gun: a small 3D model (pistol, gloved right hand, sleeve) made of
 * rounded boxes, rasterised once per frame size with a z-buffer at 2x2
 * supersampling and lit like a snowy day (sun, sky, snow bounce, metal
 * reflections, material grain). Two cached layers -- body + hand, and
 * the slide, so the slide can cycle back when it fires -- are blended
 * into every frame with recoil, sway and bob as 2D transforms.
 * =================================================================== */

typedef struct { float p[3], n[3]; } GVtx;
typedef struct { GVtx v[3]; unsigned char mat, layer; } GTri;

enum { M_POLY, M_SLIDE, M_GLOVE, M_PAD, M_SLEEVE, M_DOT, M_BARREL, M_DARK, M_CUFF };

static GTri *s_tri;
static int s_ntri, s_captri;

/* gun placement in view space (x right, y up, z forward, cm) */
static float s_gun_rot[9], s_gun_pos[3] = {6.6f, -8.0f, 21.5f};

static void m3_euler(float *m, float yaw, float pitch, float roll)
{
    float cy = cosf(yaw), sy = sinf(yaw), cp = cosf(pitch), sp = sinf(pitch);
    float cr = cosf(roll), sr = sinf(roll);
    /* R = Ry(yaw) * Rx(pitch) * Rz(roll), columns = local axes */
    m[0] = cy * cr + sy * sp * sr; m[1] = -cy * sr + sy * sp * cr; m[2] = sy * cp;
    m[3] = cp * sr;                m[4] = cp * cr;                 m[5] = -sp;
    m[6] = -sy * cr + cy * sp * sr; m[7] = sy * sr + cy * sp * cr; m[8] = cy * cp;
}

static void m3_mul(const float *m, const float *v, float *o)
{
    o[0] = m[0] * v[0] + m[1] * v[1] + m[2] * v[2];
    o[1] = m[3] * v[0] + m[4] * v[1] + m[5] * v[2];
    o[2] = m[6] * v[0] + m[7] * v[1] + m[8] * v[2];
}

/* gun-local point -> view space */
static void gun_to_view(const float *l, float *o)
{
    m3_mul(s_gun_rot, l, o);
    o[0] += s_gun_pos[0]; o[1] += s_gun_pos[1]; o[2] += s_gun_pos[2];
}

static void tri_push(const GVtx *a, const GVtx *b, const GVtx *c, int mat, int layer)
{
    if (s_ntri == s_captri) {
        s_captri = s_captri ? s_captri * 2 : 8192;
        s_tri = (GTri *)realloc(s_tri, (size_t)s_captri * sizeof *s_tri);
    }
    s_tri[s_ntri].v[0] = *a; s_tri[s_ntri].v[1] = *b; s_tri[s_ntri].v[2] = *c;
    s_tri[s_ntri].mat = (unsigned char)mat;
    s_tri[s_ntri].layer = (unsigned char)layer;
    s_ntri++;
}

/* Rounded box: centre c and half sizes h in gun space, edge radius r,
 * turned by (yaw, pitch, roll) about its centre. */
static void rbox(float cx, float cy, float cz, float hx, float hy, float hz, float r,
                 float yaw, float pitch, float roll, int mat, int layer)
{
    enum { N = 8 };
    float R[9], h[3] = {hx, hy, hz}, in[3];
    GVtx grid[N + 1][N + 1];
    int face, i, j, k;
    if (r > hx) r = hx;
    if (r > hy) r = hy;
    if (r > hz) r = hz;
    for (k = 0; k < 3; k++) in[k] = h[k] - r;
    m3_euler(R, yaw, pitch, roll);
    for (face = 0; face < 6; face++) {
        int ax = face / 2, a1 = (ax + 1) % 3, a2 = (ax + 2) % 3;
        float sgn = (face & 1) ? -1.0f : 1.0f;
        for (i = 0; i <= N; i++)
            for (j = 0; j <= N; j++) {
                float q[3], p[3], n[3], d[3], len, lp[3], ln[3];
                q[ax] = sgn * h[ax];
                q[a1] = (-1.0f + 2.0f * (float)i / N) * h[a1];
                q[a2] = (-1.0f + 2.0f * (float)j / N) * h[a2];
                for (k = 0; k < 3; k++) {
                    p[k] = q[k] < -in[k] ? -in[k] : q[k] > in[k] ? in[k] : q[k];
                    d[k] = q[k] - p[k];
                }
                len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                if (len < 1e-6f) { n[0] = n[1] = n[2] = 0; n[ax] = sgn; }
                else for (k = 0; k < 3; k++) n[k] = d[k] / len;
                for (k = 0; k < 3; k++) p[k] += n[k] * r;
                m3_mul(R, p, lp);
                lp[0] += cx; lp[1] += cy; lp[2] += cz;
                gun_to_view(lp, grid[i][j].p);
                m3_mul(R, n, ln);
                m3_mul(s_gun_rot, ln, grid[i][j].n);
            }
        for (i = 0; i < N; i++)
            for (j = 0; j < N; j++) {
                tri_push(&grid[i][j], &grid[i + 1][j], &grid[i + 1][j + 1], mat, layer);
                tri_push(&grid[i][j], &grid[i + 1][j + 1], &grid[i][j + 1], mat, layer);
            }
    }
}

/* The model. Gun space: x right, y up, z towards the muzzle; origin at the
 * slide's rear bottom edge. A compact 9 mm service pistol. */
static void build_model(void)
{
    const float G = 0.38f;               /* grip angle */
    int i;
    s_ntri = 0;
    m3_euler(s_gun_rot, -0.06f, 0.02f, -0.05f);

    /* slide (layer 1) */
    rbox(0, 1.45f, 9.3f, 1.25f, 1.45f, 9.3f, 0.45f, 0, 0, 0, M_SLIDE, 1);
    rbox(0, 3.15f, 1.3f, 1.05f, 0.42f, 0.5f, 0.12f, 0, 0, 0, M_SLIDE, 1);   /* rear sight */
    rbox(-0.62f, 3.25f, 1.32f, 0.3f, 0.36f, 0.42f, 0.08f, 0, 0, 0, M_SLIDE, 1);
    rbox(0.62f, 3.25f, 1.32f, 0.3f, 0.36f, 0.42f, 0.08f, 0, 0, 0, M_SLIDE, 1);
    rbox(-0.62f, 3.3f, 0.86f, 0.1f, 0.1f, 0.03f, 0.05f, 0, 0, 0, M_DOT, 1);
    rbox(0.62f, 3.3f, 0.86f, 0.1f, 0.1f, 0.03f, 0.05f, 0, 0, 0, M_DOT, 1);
    rbox(0, 3.15f, 17.7f, 0.2f, 0.35f, 0.4f, 0.1f, 0, 0, 0, M_SLIDE, 1);    /* front sight */
    rbox(0, 3.3f, 17.4f, 0.11f, 0.11f, 0.03f, 0.05f, 0, 0, 0, M_DOT, 1);
    rbox(1.2f, 2.05f, 10.6f, 0.12f, 0.75f, 2.2f, 0.1f, 0, 0, 0, M_DARK, 1);  /* ejection port */
    rbox(1.18f, 2.0f, 10.6f, 0.12f, 0.55f, 1.9f, 0.3f, 0, 0, 0, M_BARREL, 1);
    for (i = 0; i < 6; i++) {                                              /* serrations */
        float z = 0.9f + 0.62f * (float)i;
        rbox(1.27f, 1.35f, z, 0.04f, 1.0f, 0.14f, 0.03f, 0, 0, 0, M_DARK, 1);
        rbox(-1.27f, 1.35f, z, 0.04f, 1.0f, 0.14f, 0.03f, 0, 0, 0, M_DARK, 1);
    }
    rbox(0, 1.3f, 0.02f, 1.0f, 1.1f, 0.06f, 0.05f, 0, 0, 0, M_POLY, 1);     /* back plate */

    /* frame, barrel, trigger guard, grip (layer 0) */
    rbox(0, -0.75f, 9.0f, 1.12f, 0.8f, 8.2f, 0.35f, 0, 0, 0, M_POLY, 0);
    rbox(0, -1.55f, 14.0f, 1.0f, 0.3f, 3.0f, 0.2f, 0, 0, 0, M_POLY, 0);       /* rail */
    rbox(0, 1.35f, 18.7f, 0.62f, 0.62f, 0.25f, 0.55f, 0, 0, 0, M_BARREL, 0);  /* muzzle */
    rbox(0, -3.9f, 9.9f, 0.55f, 0.3f, 2.9f, 0.25f, 0, 0, 0, M_POLY, 0);       /* guard */
    rbox(0, -2.7f, 12.8f, 0.55f, 1.35f, 0.35f, 0.25f, 0, -0.35f, 0, M_POLY, 0);
    rbox(0, -2.6f, 9.8f, 0.3f, 1.0f, 0.18f, 0.15f, 0, 0.25f, 0, M_POLY, 0);   /* trigger */
    rbox(0, -5.9f, 3.5f, 1.4f, 5.3f, 2.85f, 0.9f, 0, G, 0, M_POLY, 0);       /* grip */
    rbox(0, -0.6f, 0.6f, 1.0f, 0.5f, 1.2f, 0.4f, 0, 0, 0, M_POLY, 0);        /* beavertail */

    /* gloved hand */
    rbox(0.55f, -6.4f, 2.4f, 2.25f, 4.4f, 3.3f, 1.8f, 0.05f, G, 0, M_GLOVE, 0);   /* palm */
    rbox(1.6f, -6.0f, 1.5f, 1.15f, 3.4f, 2.4f, 1.1f, 0.05f, G, 0.05f, M_PAD, 0); /* back pad */
    for (i = 0; i < 3; i++) {                                                 /* fingers */
        float y = -4.6f - 2.05f * (float)i, z = 6.2f + 0.75f * (float)i;
        rbox(0.25f, y, z, 2.05f, 0.92f, 0.95f, 0.9f, 0, 0.1f, 0.06f, M_GLOVE, 0);
        rbox(1.95f, y + 0.1f, z - 0.4f, 0.75f, 0.9f, 0.95f, 0.75f, 0.4f, 0.1f, 0, M_GLOVE, 0);
    }
    rbox(1.75f, -1.75f, 8.2f, 0.8f, 0.78f, 3.4f, 0.75f, -0.05f, 0.08f, 0, M_GLOVE, 0); /* index */
    rbox(1.55f, -2.2f, 11.4f, 0.7f, 0.68f, 1.3f, 0.65f, -0.2f, 0.5f, 0, M_GLOVE, 0);
    rbox(-1.85f, -0.9f, 5.0f, 0.78f, 0.82f, 3.3f, 0.75f, 0.12f, 0.05f, 0, M_GLOVE, 0); /* thumb */
    rbox(0.9f, -11.2f, -1.6f, 2.6f, 2.0f, 2.2f, 1.8f, 0.1f, 0.8f, 0, M_CUFF, 0);   /* cuff */
    rbox(1.6f, -15.5f, -8.5f, 3.6f, 3.3f, 9.5f, 3.2f, 0.18f, 0.72f, 0, M_SLEEVE, 0);
}

typedef struct {
    int w, h;                    /* sprite size (pixels) */
    uint8_t *rgba;               /* premultiplied, sRGB */
    uint8_t *flash;              /* how much the muzzle flash lights it */
    int *span0, *span1;          /* per row: first / last+1 covered pixel */
} GLayer;

typedef struct {
    int key_h, valid;
    float ox, oy;                /* screen position of sprite (0,0) relative to the centre */
    float fpx;                   /* focal length, pixels */
    GLayer L[2];
    float muzzle[2], port[2], pivot[2], slide_a[2], slide_b[2], slide_s;
} GSprite;

static GSprite s_spr[2];

static void proj(const GSprite *g, const float *v, float *o)
{
    o[0] = -g->ox + g->fpx * v[0] / v[2];
    o[1] = -g->oy - g->fpx * v[1] / v[2];
}

static float hash01(uint32_t x)
{
    x ^= x >> 16; x *= 0x7FEB352Du; x ^= x >> 15; x *= 0x846CA68Bu; x ^= x >> 16;
    return (float)(x & 0xFFFFFF) / 16777216.0f;
}

static const float s_sun[3] = {-0.42f, 0.78f, -0.46f};
static float s_muzzle_v[3];

static void shade(int mat, const float *P, const float *Nin, uint32_t seed, float *out, float *flash)
{
    static const struct { float a[3], f0, gloss, rough, grain; } M[] = {
        {{0.020f, 0.020f, 0.022f}, 0.040f, 24, 0.60f, 0.12f},   /* polymer */
        {{0.022f, 0.023f, 0.026f}, 0.12f, 60, 0.30f, 0.05f},    /* slide */
        {{0.030f, 0.030f, 0.032f}, 0.025f, 6, 0.95f, 0.18f},    /* glove */
        {{0.045f, 0.040f, 0.036f}, 0.03f, 10, 0.85f, 0.15f},    /* pad */
        {{0.035f, 0.075f, 0.200f}, 0.05f, 16, 0.7f, 0.25f},     /* sleeve */
        {{0.85f, 0.85f, 0.80f}, 0.04f, 20, 0.6f, 0.0f},         /* sight dot */
        {{0.20f, 0.20f, 0.21f}, 0.55f, 90, 0.15f, 0.06f},       /* barrel */
        {{0.012f, 0.012f, 0.013f}, 0.03f, 10, 0.9f, 0.1f},      /* dark */
        {{0.030f, 0.030f, 0.034f}, 0.03f, 6, 0.9f, 0.5f},       /* cuff */
    };
    float N[3], V[3], L[3], H[3], len, ndl, ndv, ndh, F, hemi, gr, env[3], Rv[3], spec, t;
    int k;
    len = sqrtf(Nin[0] * Nin[0] + Nin[1] * Nin[1] + Nin[2] * Nin[2]) + 1e-9f;
    for (k = 0; k < 3; k++) N[k] = Nin[k] / len;
    len = sqrtf(P[0] * P[0] + P[1] * P[1] + P[2] * P[2]) + 1e-9f;
    for (k = 0; k < 3; k++) V[k] = -P[k] / len;
    len = sqrtf(s_sun[0] * s_sun[0] + s_sun[1] * s_sun[1] + s_sun[2] * s_sun[2]);
    for (k = 0; k < 3; k++) L[k] = s_sun[k] / len;
    ndv = N[0] * V[0] + N[1] * V[1] + N[2] * V[2];
    if (ndv < 0) { for (k = 0; k < 3; k++) N[k] = -N[k]; ndv = -ndv; }
    ndl = N[0] * L[0] + N[1] * L[1] + N[2] * L[2];
    if (ndl < 0) ndl = 0;
    for (k = 0; k < 3; k++) H[k] = L[k] + V[k];
    len = sqrtf(H[0] * H[0] + H[1] * H[1] + H[2] * H[2]) + 1e-9f;
    ndh = (N[0] * H[0] + N[1] * H[1] + N[2] * H[2]) / len;
    if (ndh < 0) ndh = 0;
    F = M[mat].f0 + (1.0f - M[mat].f0) * powf(1.0f - ndv, 5.0f) * (1.0f - 0.6f * M[mat].rough);
    gr = 1.0f + M[mat].grain * (hash01(seed) - 0.5f) * 2.0f;
    hemi = 0.5f + 0.5f * N[1];
    /* reflection of the surroundings: snow below and at the horizon, sky above */
    for (k = 0; k < 3; k++) Rv[k] = 2.0f * ndv * N[k] - V[k];
    {
        static const float sky[3] = {0.30f, 0.45f, 0.80f}, hor[3] = {0.80f, 0.84f, 0.92f},
                           snow[3] = {0.75f, 0.78f, 0.86f}, avg[3] = {0.55f, 0.62f, 0.78f};
        t = Rv[1] > 0 ? sqrtf(Rv[1]) : 0;
        for (k = 0; k < 3; k++) {
            env[k] = Rv[1] > 0 ? hor[k] + (sky[k] - hor[k]) * t : snow[k];
            env[k] += (avg[k] - env[k]) * M[mat].rough;
        }
    }
    spec = powf(ndh, M[mat].gloss) * (M[mat].gloss + 8.0f) / 25.0f;
    for (k = 0; k < 3; k++) {
        static const float sunc[3] = {1.65f, 1.60f, 1.50f};
        static const float skyc[3] = {0.40f, 0.50f, 0.72f}, bounce[3] = {0.62f, 0.64f, 0.72f};
        float amb = bounce[k] + (skyc[k] - bounce[k]) * hemi;
        float diff = M[mat].a[k] * gr * (amb * 0.8f + sunc[k] * ndl);
        float sp = sunc[k] * spec * F * (1.0f - 0.7f * M[mat].rough)
                   + env[k] * F * 0.55f * (1.0f - 0.6f * M[mat].rough);
        out[k] = diff + sp;
    }
    /* light from the muzzle flash */
    {
        float d[3], dl;
        for (k = 0; k < 3; k++) d[k] = s_muzzle_v[k] - P[k];
        dl = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) + 1e-6f;
        t = (N[0] * d[0] + N[1] * d[1] + N[2] * d[2]) / dl;
        *flash = (t > 0 ? t : 0) / (1.0f + (dl / 3.5f) * (dl / 3.5f));
        if (*flash > 1) *flash = 1;
    }
}

static uint8_t to_srgb8(float v)
{
    v = v / (1.0f + 0.12f * v);                /* soft shoulder */
    if (v <= 0) return 0;
    if (v >= 1) return 255;
    return (uint8_t)(powf(v, 1.0f / 2.2f) * 255.0f + 0.5f);
}

/* Render one layer into the sprite: 2x2 supersampled, z-buffered. */
static void render_layer(GSprite *g, int layer)
{
    enum { S = 2 };
    int W = g->L[layer].w * S, Hh = g->L[layer].h * S, t, x, y;
    float *zb = (float *)malloc((size_t)W * Hh * sizeof(float));
    float *col = (float *)calloc((size_t)W * Hh * 4, sizeof(float));
    float *fl = (float *)calloc((size_t)W * Hh, sizeof(float));
    GLayer *L = &g->L[layer];
    if (!zb || !col || !fl) { free(zb); free(col); free(fl); return; }
    for (x = 0; x < W * Hh; x++) zb[x] = 1e30f;
    for (t = 0; t < s_ntri; t++) {
        const GTri *tr = &s_tri[t];
        float sx[3], sy[3], iz[3], area, minx, maxx, miny, maxy;
        int k, x0, x1, y0, y1;
        if (tr->layer != layer) continue;
        for (k = 0; k < 3; k++) {
            float s2[2];
            if (tr->v[k].p[2] < 3.0f) break;
            proj(g, tr->v[k].p, s2);
            sx[k] = s2[0] * S; sy[k] = s2[1] * S;
            iz[k] = 1.0f / tr->v[k].p[2];
        }
        if (k < 3) continue;
        area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
        if (fabsf(area) < 1e-6f) continue;
        minx = fminf(sx[0], fminf(sx[1], sx[2])); maxx = fmaxf(sx[0], fmaxf(sx[1], sx[2]));
        miny = fminf(sy[0], fminf(sy[1], sy[2])); maxy = fmaxf(sy[0], fmaxf(sy[1], sy[2]));
        x0 = (int)floorf(minx); x1 = (int)ceilf(maxx);
        y0 = (int)floorf(miny); y1 = (int)ceilf(maxy);
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > W) x1 = W;
        if (y1 > Hh) y1 = Hh;
        for (y = y0; y < y1; y++)
            for (x = x0; x < x1; x++) {
                float px = (float)x + 0.5f, py = (float)y + 0.5f, b0, b1, b2, w0, w1, w2, wz, z;
                b0 = ((sx[1] - px) * (sy[2] - py) - (sx[2] - px) * (sy[1] - py)) / area;
                b1 = ((sx[2] - px) * (sy[0] - py) - (sx[0] - px) * (sy[2] - py)) / area;
                b2 = 1.0f - b0 - b1;
                if (b0 < 0 || b1 < 0 || b2 < 0) continue;
                w0 = b0 * iz[0]; w1 = b1 * iz[1]; w2 = b2 * iz[2];
                wz = w0 + w1 + w2;
                z = 1.0f / wz;
                if (z >= zb[(size_t)y * W + x]) continue;
                zb[(size_t)y * W + x] = z;
                {
                    float P[3], N[3], c[3], f;
                    int j;
                    w0 /= wz; w1 /= wz; w2 /= wz;
                    for (j = 0; j < 3; j++) {
                        P[j] = tr->v[0].p[j] * w0 + tr->v[1].p[j] * w1 + tr->v[2].p[j] * w2;
                        N[j] = tr->v[0].n[j] * w0 + tr->v[1].n[j] * w1 + tr->v[2].n[j] * w2;
                    }
                    shade(tr->mat, P, N, (uint32_t)(x / 2) * 7919u + (uint32_t)(y / 2) * 104729u
                          + tr->mat * 31u, c, &f);
                    col[((size_t)y * W + x) * 4 + 0] = c[0];
                    col[((size_t)y * W + x) * 4 + 1] = c[1];
                    col[((size_t)y * W + x) * 4 + 2] = c[2];
                    col[((size_t)y * W + x) * 4 + 3] = 1.0f;
                    fl[(size_t)y * W + x] = f;
                }
            }
    }
    /* contact shading: darken samples just behind a nearer neighbour */
    for (y = 1; y < Hh - 1; y++)
        for (x = 1; x < W - 1; x++) {
            size_t i = (size_t)y * W + x;
            float z = zb[i], occ = 0;
            int dx, dy;
            if (z >= 1e29f) continue;
            for (dy = -1; dy <= 1; dy++)
                for (dx = -1; dx <= 1; dx++) {
                    float zn = zb[i + (ptrdiff_t)dy * W + dx];
                    if (zn < z - 0.6f) occ += 1.0f;
                }
            if (occ > 0) {
                float k = 1.0f - 0.07f * occ;
                col[i * 4] *= k; col[i * 4 + 1] *= k; col[i * 4 + 2] *= k;
            }
        }
    /* downsample */
    for (y = 0; y < L->h; y++) {
        int s0 = L->w, s1 = 0;
        for (x = 0; x < L->w; x++) {
            float c[4] = {0, 0, 0, 0}, f = 0;
            int dx, dy, k;
            for (dy = 0; dy < S; dy++)
                for (dx = 0; dx < S; dx++) {
                    size_t i = (size_t)(y * S + dy) * W + (x * S + dx);
                    for (k = 0; k < 4; k++) c[k] += col[i * 4 + k];
                    f += fl[i];
                }
            for (k = 0; k < 4; k++) c[k] /= S * S;
            f /= S * S;
            {
                uint8_t *o = L->rgba + ((size_t)y * L->w + x) * 4;
                float a = c[3];
                if (a > 0.001f) {
                    o[0] = (uint8_t)(to_srgb8(c[0] / a) * a + 0.5f);   /* premultiplied */
                    o[1] = (uint8_t)(to_srgb8(c[1] / a) * a + 0.5f);
                    o[2] = (uint8_t)(to_srgb8(c[2] / a) * a + 0.5f);
                    o[3] = (uint8_t)(a * 255.0f + 0.5f);
                    if (x < s0) s0 = x;
                    s1 = x + 1;
                } else
                    o[0] = o[1] = o[2] = o[3] = 0;
                L->flash[(size_t)y * L->w + x] = (uint8_t)(f * a * 255.0f);
            }
        }
        L->span0[y] = s0;
        L->span1[y] = s1;
    }
    free(zb); free(col); free(fl);
}

static GSprite *gun_sprite(int h)
{
    GSprite *g;
    int i, l;
    for (i = 0; i < 2; i++)
        if (s_spr[i].valid && s_spr[i].key_h == h)
            return &s_spr[i];
    g = &s_spr[s_spr[0].valid && !s_spr[1].valid ? 1 : 0];
    for (l = 0; l < 2; l++) {
        free(g->L[l].rgba); free(g->L[l].flash); free(g->L[l].span0); free(g->L[l].span1);
    }
    memset(g, 0, sizeof *g);
    g->key_h = h;
    g->fpx = (float)h * 0.80f;           /* ~64 degree vertical view-model fov */
    g->ox = -0.18f * (float)h;           /* sprite covers centre-0.18h .. +1.05h */
    g->oy = -0.02f * (float)h;           /*            and centre-0.02h .. +0.62h */
    build_model();
    {
        float lm[3] = {0, 1.35f, 18.95f}, lp[3] = {1.35f, 2.1f, 10.6f}, lg[3] = {0.5f, -5.5f, 3.0f};
        float la[3] = {0, 2.0f, 0.0f}, lb[3] = {0, 2.0f, -2.6f}, v[3], va[3];
        gun_to_view(lm, s_muzzle_v);
        proj(g, s_muzzle_v, g->muzzle);
        gun_to_view(lp, v); proj(g, v, g->port);
        gun_to_view(lg, v); proj(g, v, g->pivot);
        gun_to_view(la, va); proj(g, va, g->slide_a);
        gun_to_view(lb, v); proj(g, v, g->slide_b);
        g->slide_s = va[2] / v[2];
    }
    for (l = 0; l < 2; l++) {
        GLayer *L = &g->L[l];
        L->w = (int)(1.23f * (float)h);
        L->h = (int)(0.64f * (float)h);
        L->rgba = (uint8_t *)calloc((size_t)L->w * L->h, 4);
        L->flash = (uint8_t *)calloc((size_t)L->w * L->h, 1);
        L->span0 = (int *)calloc((size_t)L->h, sizeof(int));
        L->span1 = (int *)calloc((size_t)L->h, sizeof(int));
        if (!L->rgba || !L->flash || !L->span0 || !L->span1)
            return NULL;
        render_layer(g, l);
    }
    g->valid = 1;
    fprintf(stderr, "[FPS] gun model rendered for %d lines (%d triangles)\n", h, s_ntri);
    return g;
}

/* ------------------------------------------------------- frame drawing */

static uint32_t *s_px;
static int s_w, s_h, s_stride;
static float s_cx, s_cy, s_sqx;          /* centre; horizontal squeeze (4:3 copy of a wide frame) */

static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

static void fill_rect(int x0, int y0, int x1, int y1, uint32_t c)
{
    int x, y;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > s_w) x1 = s_w;
    if (y1 > s_h) y1 = s_h;
    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++)
            s_px[(size_t)y * s_stride + x] = c;
}

static const unsigned char s_digits[10][7] = {
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
    {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E},
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}};
static const unsigned char s_hits_txt[4][7] = {          /* H I T S */
    {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}, {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E},
    {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}, {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}};

static void glyph(const unsigned char *g, int x0, int y0, int sc, uint32_t c)
{
    int r, k;
    for (r = 0; r < 7; r++)
        for (k = 0; k < 5; k++)
            if (g[r] & (0x10 >> k))
                fill_rect(x0 + k * sc, y0 + r * sc, x0 + (k + 1) * sc, y0 + (r + 1) * sc, c);
}

/* Blend a cached layer with a 2D transform: screen = T + A (sprite - P). */
static void blit_layer(const GLayer *L, const float A[4], const float T[2], const float P[2],
                       float tint, float flash, const float flash_col[3])
{
    float det = A[0] * A[3] - A[1] * A[2], I[4], cx[4], cy[4];
    int k, x, y, x0, x1, y0, y1;
    if (fabsf(det) < 1e-6f) return;
    I[0] = A[3] / det; I[1] = -A[1] / det; I[2] = -A[2] / det; I[3] = A[0] / det;
    for (k = 0; k < 4; k++) {
        float u = (k & 1) ? (float)L->w : 0, v = (k & 2) ? (float)L->h : 0;
        cx[k] = T[0] + A[0] * (u - P[0]) + A[1] * (v - P[1]);
        cy[k] = T[1] + A[2] * (u - P[0]) + A[3] * (v - P[1]);
    }
    x0 = (int)floorf(fminf(fminf(cx[0], cx[1]), fminf(cx[2], cx[3])));
    x1 = (int)ceilf(fmaxf(fmaxf(cx[0], cx[1]), fmaxf(cx[2], cx[3])));
    y0 = (int)floorf(fminf(fminf(cy[0], cy[1]), fminf(cy[2], cy[3])));
    y1 = (int)ceilf(fmaxf(fmaxf(cy[0], cy[1]), fmaxf(cy[2], cy[3])));
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > s_w) x1 = s_w;
    if (y1 > s_h) y1 = s_h;
    for (y = y0; y < y1; y++) {
        float dy = (float)y + 0.5f - T[1];
        for (x = x0; x < x1; x++) {
            float dx = (float)x + 0.5f - T[0];
            float u = P[0] + I[0] * dx + I[1] * dy - 0.5f, v = P[1] + I[2] * dx + I[3] * dy - 0.5f;
            int iu = (int)floorf(u), iv = (int)floorf(v), j;
            float fu = u - (float)iu, fv = v - (float)iv, c[4] = {0, 0, 0, 0}, fm = 0, wsum[4];
            const uint8_t *p[4];
            const int du[4] = {0, 1, 0, 1}, dv[4] = {0, 0, 1, 1};
            if (iv < -1 || iv >= L->h || iu < -1 || iu >= L->w) continue;
            if (iv >= 0 && (iu + 1 < L->span0[iv] || iu >= L->span1[iv])
                    && (iv + 1 >= L->h || iu + 1 < L->span0[iv + 1] || iu >= L->span1[iv + 1]))
                continue;
            wsum[0] = (1 - fu) * (1 - fv); wsum[1] = fu * (1 - fv);
            wsum[2] = (1 - fu) * fv;       wsum[3] = fu * fv;
            for (j = 0; j < 4; j++) {
                int su = iu + du[j], sv = iv + dv[j];
                if (su < 0 || sv < 0 || su >= L->w || sv >= L->h) { p[j] = NULL; continue; }
                p[j] = L->rgba + ((size_t)sv * L->w + su) * 4;
                c[0] += p[j][0] * wsum[j]; c[1] += p[j][1] * wsum[j];
                c[2] += p[j][2] * wsum[j]; c[3] += p[j][3] * wsum[j];
                fm += L->flash[(size_t)sv * L->w + su] * wsum[j];
            }
            if (c[3] < 0.5f) continue;
            {
                uint32_t *d = &s_px[(size_t)y * s_stride + x], o = *d;
                float a = c[3] / 255.0f, fl = flash * fm / 255.0f;
                float r = c[0] * tint + fl * flash_col[0] * 255.0f;
                float gg = c[1] * tint + fl * flash_col[1] * 255.0f;
                float b = c[2] * tint + fl * flash_col[2] * 255.0f;
                r += (float)((o >> 16) & 255) * (1 - a);
                gg += (float)((o >> 8) & 255) * (1 - a);
                b += (float)(o & 255) * (1 - a);
                *d = 0xFF000000u | ((uint32_t)fminf(r, 255.0f) << 16)
                     | ((uint32_t)fminf(gg, 255.0f) << 8) | (uint32_t)fminf(b, 255.0f);
            }
        }
    }
}

/* Additive light (screen blend) of a soft shape, evaluated per pixel. */
typedef float (*shape_fn)(float dx, float dy, const void *arg);

static void add_light(float cxp, float cyp, float rad, shape_fn fn, const void *arg,
                      const float col[3], float gain)
{
    int x, y, x0 = (int)(cxp - rad * s_sqx), x1 = (int)(cxp + rad * s_sqx) + 1;
    int y0 = (int)(cyp - rad), y1 = (int)(cyp + rad) + 1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > s_w) x1 = s_w;
    if (y1 > s_h) y1 = s_h;
    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++) {
            float i = fn(((float)x + 0.5f - cxp) / s_sqx, (float)y + 0.5f - cyp, arg) * gain;
            uint32_t *d, o;
            float r, g, b;
            if (i < 0.003f) continue;
            d = &s_px[(size_t)y * s_stride + x];
            o = *d;
            r = (float)((o >> 16) & 255) / 255.0f;
            g = (float)((o >> 8) & 255) / 255.0f;
            b = (float)(o & 255) / 255.0f;
            r = 1 - (1 - r) * (1 - clamp01(i * col[0]));
            g = 1 - (1 - g) * (1 - clamp01(i * col[1]));
            b = 1 - (1 - b) * (1 - clamp01(i * col[2]));
            *d = 0xFF000000u | ((uint32_t)(r * 255.0f) << 16) | ((uint32_t)(g * 255.0f) << 8)
                 | (uint32_t)(b * 255.0f);
        }
}

/* Alpha-blended soft disc (smoke, mist). */
static void soft_disc(float cxp, float cyp, float rad, const float col[3], float alpha)
{
    int x, y, x0 = (int)(cxp - rad * s_sqx), x1 = (int)(cxp + rad * s_sqx) + 1;
    int y0 = (int)(cyp - rad), y1 = (int)(cyp + rad) + 1;
    if (rad < 0.5f || alpha < 0.004f) return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > s_w) x1 = s_w;
    if (y1 > s_h) y1 = s_h;
    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++) {
            float dx = ((float)x + 0.5f - cxp) / s_sqx / rad, dy = ((float)y + 0.5f - cyp) / rad;
            float q = dx * dx + dy * dy, a;
            uint32_t *d, o;
            if (q >= 1) continue;
            a = alpha * (1 - q) * (1 - q);
            d = &s_px[(size_t)y * s_stride + x];
            o = *d;
            *d = 0xFF000000u
                 | ((uint32_t)(((o >> 16) & 255) * (1 - a) + col[0] * 255.0f * a) << 16)
                 | ((uint32_t)(((o >> 8) & 255) * (1 - a) + col[1] * 255.0f * a) << 8)
                 | (uint32_t)((o & 255) * (1 - a) + col[2] * 255.0f * a);
        }
}

typedef struct { float rot, petals, len[6], ang[6], core, glow; } FlashShape;

static float flash_fn(float dx, float dy, const void *arg)
{
    const FlashShape *f = (const FlashShape *)arg;
    float r = sqrtf(dx * dx + dy * dy), a = atan2f(dy, dx), v;
    int k;
    v = expf(-(r * r) / (f->core * f->core)) * 1.6f;          /* white core */
    v += expf(-r / f->glow) * 0.45f;                           /* warm glow */
    for (k = 0; k < (int)f->petals; k++) {
        float da = a - f->ang[k], along, across;
        while (da > 3.14159265f) da -= 6.2831853f;
        while (da < -3.14159265f) da += 6.2831853f;
        along = r * cosf(da);
        across = r * sinf(da);
        if (along <= 0) continue;
        v += expf(-along / f->len[k]) * expf(-(across * across)
              / (0.025f * along * along + f->core * f->core * 0.15f)) * 1.3f;
    }
    return v;
}

static float glow_fn(float dx, float dy, const void *arg)
{
    float r2 = dx * dx + dy * dy, s = *(const float *)arg;
    return expf(-r2 / (s * s));
}

/* View-space projection of a world point with the camera the tick used. */
extern int ssx3_WidescreenNow(void);
extern float ssx3_Aspect(void);

static int world_to_screen(const float *p, float *sx, float *sy, float *depth)
{
    float d[3], z, x, y, aspect = ssx3_WidescreenNow() ? ssx3_Aspect() : 4.0f / 3.0f;
    const float tanv = 0.776f;           /* the race camera's 1/m11 */
    int k;
    for (k = 0; k < 3; k++) d[k] = p[k] - s_cam_eye[k];
    z = d[0] * s_cam_f[0] + d[1] * s_cam_f[1] + d[2] * s_cam_f[2];
    if (z < 60.0f) return 0;
    x = (d[0] * s_cam_r[0] + d[1] * s_cam_r[1] + d[2] * s_cam_r[2]) / z;
    y = (d[0] * s_cam_u[0] + d[1] * s_cam_u[1] + d[2] * s_cam_u[2]) / z;
    *sx = s_cx + x / (tanv * aspect) * (float)s_w * 0.5f;
    *sy = s_cy - y / tanv * (float)s_h * 0.5f;
    *depth = z;
    return 1;
}


static void draw_crosshair(uint32_t since_shot, uint32_t since_hit)
{
    int th = s_h / 450 + 1, len = s_h / 48 + 2, k, o;
    int gap = s_h / 160 + 2 + (since_shot < 250 ? (int)((250 - since_shot) * s_h / 250 / 45) : 0);
    int cx = (int)s_cx, cy = (int)s_cy;
    for (o = 0; o < 2; o++) {
        uint32_t c = o ? 0xFFF2F2F2u : 0xFF101010u;
        int e = o ? 0 : 1;
        fill_rect(cx - th / 2 - e, cy - th / 2 - e, cx - th / 2 + th + e, cy - th / 2 + th + e, c);
        fill_rect(cx - gap - len - e, cy - th / 2 - e, cx - gap + e, cy - th / 2 + th + e, c);
        fill_rect(cx + gap - e, cy - th / 2 - e, cx + gap + len + e, cy - th / 2 + th + e, c);
        fill_rect(cx - th / 2 - e, cy + gap - e, cx - th / 2 + th + e, cy + gap + len + e, c);
    }
    if (since_hit < 180) {                       /* hit ticks */
        int r0 = gap + 2, r1 = gap + len + 2;
        for (k = r0; k < r1; k++) {
            uint32_t c = 0xFFFFFFFFu;
            fill_rect(cx + k, cy + k, cx + k + th, cy + k + th, c);
            fill_rect(cx - k - th, cy + k, cx - k, cy + k + th, c);
            fill_rect(cx + k, cy - k - th, cx + k + th, cy - k, c);
            fill_rect(cx - k - th, cy - k - th, cx - k, cy - k, c);
        }
    }
}

/* Drawn into each flipped frame by the Vulkan backend's flip thread
 * (stride in pixels). */
void ssx3_FpsOverlay(uint32_t *px, int w, int h, int stride)
{
    static uint32_t last_flashed = 0;
    GSprite *g;
    uint32_t now, n, since_shot = 100000u, since_hit = 100000u;
    float A[4], T[2], tint, kick = 0, flash = 0, slide = 0, aspect, rot;
    const Shot *last = NULL;
    int i, k;

    if (!s_active || !px || w < 160 || h < 120)
        return;
    s_px = px; s_w = w; s_h = h; s_stride = stride;
    s_cx = (float)w * 0.5f; s_cy = (float)h * 0.5f;
    aspect = ssx3_WidescreenNow() ? ssx3_Aspect() : 4.0f / 3.0f;
    s_sqx = ((float)w / (float)h) / aspect;
    if (s_sqx > 1.01f) s_sqx = 1.0f;
    now = xbox_TickCountMs();
    n = s_nshots;
    if (n) {
        last = &s_shots[(n - 1) & 15];
        since_shot = now - last->t;
    }
    for (i = 0; i < 16 && i < (int)n; i++) {
        const Shot *s = &s_shots[(n - 1 - i) & 15];
        if (s->hit && now - s->t < since_hit) since_hit = now - s->t;
    }

    g = gun_sprite(h);
    if (!g) return;

    /* scene brightness: the gun follows the exposure of the frame a bit */
    {
        float sum = 0;
        int cnt = 0;
        for (i = 0; i < 8; i++)
            for (k = 0; k < 8; k++) {
                uint32_t o = px[(size_t)(h / 10 + i * h / 10) * stride + w / 10 + k * w / 10];
                sum += (float)(((o >> 16) & 255) + ((o >> 8) & 255) + (o & 255)) / 765.0f;
                cnt++;
            }
        tint = 0.62f + 0.38f * sum / (float)cnt;
    }

    /* recoil: a sharp kick, then a damped return; the slide cycles */
    if (since_shot < 600) {
        float t = (float)since_shot / 1000.0f;
        kick = t < 0.018f ? t / 0.018f : expf(-(t - 0.018f) / 0.075f) * cosf((t - 0.018f) * 18.0f);
        slide = t < 0.012f ? t / 0.012f : t < 0.06f ? 1.0f - (t - 0.012f) / 0.048f : 0.0f;
        flash = since_shot < 45 ? 1.0f - (float)since_shot / 60.0f : 0.0f;
        if (last && n != last_flashed) {         /* never miss the flash frame */
            if (flash < 0.8f) flash = 0.8f;
            last_flashed = n;
        }
    }
    rot = -0.11f * kick + s_roll;
    A[0] = cosf(rot) * s_sqx; A[1] = -sinf(rot) * s_sqx;
    A[2] = sinf(rot);         A[3] = cosf(rot);
    T[0] = s_cx + (g->ox + g->pivot[0] + s_sway_x * (float)h + kick * 0.012f * (float)h) * s_sqx;
    T[1] = s_cy + g->oy + g->pivot[1] + (s_sway_y + s_bob) * (float)h + kick * 0.035f * (float)h;
    {
        static const float fcol[3] = {1.0f, 0.62f, 0.28f};
        float As[4], Ts[2], m[2], ms[2], sc;
        blit_layer(&g->L[0], A, T, g->pivot, tint, flash * 0.55f, fcol);
        /* the slide: moved back towards the eye (along slide_a -> slide_b,
         * growing by slide_s) */
        sc = 1.0f + slide * (g->slide_s - 1.0f);
        for (k = 0; k < 4; k++) As[k] = A[k] * sc;
        m[0] = g->slide_a[0] + slide * (g->slide_b[0] - g->slide_a[0]);
        m[1] = g->slide_a[1] + slide * (g->slide_b[1] - g->slide_a[1]);
        ms[0] = T[0] + A[0] * (m[0] - g->pivot[0]) + A[1] * (m[1] - g->pivot[1]);
        ms[1] = T[1] + A[2] * (m[0] - g->pivot[0]) + A[3] * (m[1] - g->pivot[1]);
        Ts[0] = ms[0]; Ts[1] = ms[1];
        blit_layer(&g->L[1], As, Ts, g->slide_a, tint, flash * 0.55f, fcol);
    }

    /* muzzle flash, brass, smoke: placed by the transformed gun */
#define GUN_X(u, v) (T[0] + A[0] * ((u) - g->pivot[0]) + A[1] * ((v) - g->pivot[1]))
#define GUN_Y(u, v) (T[1] + A[2] * ((u) - g->pivot[0]) + A[3] * ((v) - g->pivot[1]))
    {
        float mx = GUN_X(g->muzzle[0], g->muzzle[1]), my = GUN_Y(g->muzzle[0], g->muzzle[1]);
        float px_ = GUN_X(g->port[0], g->port[1]), py_ = GUN_Y(g->port[0], g->port[1]);
        for (i = 0; i < 6 && i < (int)n; i++) {
            const Shot *s = &s_shots[(n - 1 - i) & 15];
            float age = (float)(now - s->t) / 1000.0f;
            uint32_t sd = s->seed;
            /* smoke: a short puff that the rider's speed sweeps back past the eye */
            if (age < 0.45f) {
                for (k = 0; k < 4; k++) {
                    float a = age + 0.02f * (float)k, life = clamp01(1.0f - a / 0.45f);
                    float ex = (hash01(sd + k * 3u) - 0.4f) * 0.06f * (float)h;
                    float ey = (hash01(sd + k * 5u + 1u) - 0.7f) * 0.05f * (float)h;
                    float sx = mx + (ex + a * (0.10f + 0.10f * (float)k) * (float)h) * s_sqx;
                    float sy = my + ey * a * 4.0f + a * (0.25f + 0.08f * (float)k) * (float)h;
                    float rad = (0.012f + a * (0.18f + 0.05f * (float)k)) * (float)h;
                    static const float sc[3] = {0.80f, 0.81f, 0.84f};
                    if (a < 0.02f) continue;
                    soft_disc(sx, sy, rad, sc, 0.20f * life * life * tint);
                }
            }
            /* brass: thrown up and right, tumbling, gone in half a second */
            if (age < 0.5f) {
                float vx = (0.75f + 0.35f * hash01(sd + 11u)) * (float)h;
                float vy = -(0.85f + 0.3f * hash01(sd + 12u)) * (float)h;
                float bx = px_ + (vx * age + 0.25f * (float)h * age * age) * s_sqx;
                float by = py_ + vy * age + 2.6f * (float)h * age * age;
                float ang = hash01(sd + 13u) * 6.28f + age * 31.0f;
                float len = (0.022f + age * 0.05f) * (float)h, wid = len * 0.36f;
                float ca = cosf(ang), sa = sinf(ang);
                int seg;
                for (seg = 0; seg < 12; seg++) {     /* a shaded capsule */
                    float tt = (float)seg / 11.0f - 0.5f;
                    float cxp = bx + ca * len * tt * s_sqx, cyp = by + sa * len * tt;
                    float lit = 0.55f + 0.45f * cosf((float)seg * 0.25f + ang);
                    float bc[3] = {0.80f * lit + 0.15f, 0.58f * lit + 0.08f, 0.22f * lit};
                    if (seg == 11) { bc[0] *= 0.6f; bc[1] *= 0.6f; bc[2] *= 0.6f; }
                    soft_disc(cxp, cyp, wid * 0.62f, bc, 0.95f);
                }
            }
        }
        /* the flash itself, different every shot */
        if (flash > 0 && last) {
            FlashShape f;
            static const float core_c[3] = {1.0f, 0.92f, 0.72f}, warm[3] = {1.0f, 0.55f, 0.18f};
            float glow_s = 0.11f * (float)h;
            f.petals = 4 + (int)(hash01(last->seed + 21u) * 3.0f);
            for (k = 0; k < 6; k++) {
                f.ang[k] = hash01(last->seed + 30u + k) * 6.2831853f;
                f.len[k] = (0.03f + 0.07f * hash01(last->seed + 40u + k)) * (float)h * (0.6f + 0.4f * flash);
            }
            f.core = 0.024f * (float)h * (0.7f + 0.3f * flash);
            f.glow = 0.03f * (float)h;
            f.rot = 0;
            {
                float vx = s_cx - mx, vy = s_cy - my, vl = sqrtf(vx * vx + vy * vy) + 1e-3f;
                mx += vx / vl * 0.022f * (float)h;
                my += vy / vl * 0.022f * (float)h;
            }
            add_light(mx, my, 3.0f * glow_s, glow_fn, &glow_s, warm, 0.30f * flash);
            add_light(mx, my, 0.32f * (float)h, flash_fn, &f, core_c, flash);
        }
    }
#undef GUN_X
#undef GUN_Y

    /* hits: a burst of mist and snow where the bullet struck */
    for (i = 0; i < 16 && i < (int)n; i++) {
        const Shot *s = &s_shots[(n - 1 - i) & 15];
        float age = (float)(now - s->t) / 1000.0f, sx, sy, z;
        if (!s->hit || age > 0.7f) continue;
        if (!world_to_screen(s->hp, &sx, &sy, &z)) continue;
        {
            float unit = (float)h * 0.5f / 0.776f / z;     /* pixels per world unit */
            static const float mist[3] = {0.55f, 0.06f, 0.05f}, snow[3] = {0.92f, 0.94f, 0.98f};
            for (k = 0; k < 7; k++) {
                float a = hash01(s->seed + 50u + k) * 6.2831853f;
                float sp = (60.0f + 140.0f * hash01(s->seed + 60u + k)) * age;
                float ox = cosf(a) * sp, oy = sinf(a) * sp * 0.6f - 160.0f * age * age;
                float rad = (12.0f + 70.0f * age) * unit;
                if (rad > 0.09f * (float)h) rad = 0.09f * (float)h;
                const float *c = k < 4 ? mist : snow;
                float u2 = unit > 0.09f * (float)h / 40.0f ? 0.09f * (float)h / 40.0f : unit;
                soft_disc(sx + ox * u2 * s_sqx, sy - oy * u2, rad, c,
                          (k < 4 ? 0.55f : 0.35f) * clamp01(1.0f - age / 0.7f));
            }
            if (age < 0.05f) {
                float gs = 18.0f * unit > 0.05f * (float)h ? 0.05f * (float)h : 18.0f * unit;
                static const float wc[3] = {1.0f, 0.95f, 0.85f};
                add_light(sx, sy, gs * 3, glow_fn, &gs, wc, 0.6f);
            }
        }
    }

    draw_crosshair(since_shot, since_hit);

    /* RECOMP_FPS_SHOTS=<dir>: the first frames with a muzzle flash, as PPM
     * (a test aid: the flash lasts two or three frames). */
    if (flash > 0 && w > 640) {
        static int dumped = -1;
        static const char *dir;
        if (dumped < 0) { dir = getenv("RECOMP_FPS_SHOTS"); dumped = 0; }
        if (dir && dumped < 6) {
            char path[512];
            FILE *fp;
            snprintf(path, sizeof path, "%s/flash%d.ppm", dir, dumped++);
            if ((fp = fopen(path, "wb")) != NULL) {
                int yy, xx;
                fprintf(fp, "P6\n%d %d\n255\n", w, h);
                for (yy = 0; yy < h; yy++)
                    for (xx = 0; xx < w; xx++) {
                        uint32_t o = px[(size_t)yy * stride + xx];
                        unsigned char rgb[3] = {(unsigned char)(o >> 16), (unsigned char)(o >> 8), (unsigned char)o};
                        fwrite(rgb, 1, 3, fp);
                    }
                fclose(fp);
            }
        }
    }

    /* hit count, top centre under the race clock */
    {
        int digits[6], nd = 0, gs = h / 200 + 1, x, y = h * 18 / 100;
        int v = s_hits;
        do { digits[nd++] = v % 10; v /= 10; } while (v && nd < 6);
        x = w / 2 - (4 + 1 + nd) * 6 * gs / 2;
        for (i = 0; i < 4; i++) {
            glyph(s_hits_txt[i], x + i * 6 * gs + gs / 2, y + gs / 2, gs, 0xFF000000u);
            glyph(s_hits_txt[i], x + i * 6 * gs, y, gs, 0xFFFFFFFFu);
        }
        for (i = 0; i < nd; i++) {
            glyph(s_digits[digits[nd - 1 - i]], x + (5 + i) * 6 * gs + gs / 2, y + gs / 2, gs, 0xFF000000u);
            glyph(s_digits[digits[nd - 1 - i]], x + (5 + i) * 6 * gs, y, gs, 0xFFFFD040u);
        }
    }
}

/* ------------------------------------------------------------- sound */

/* A 9 mm shot outdoors, synthesised once: the supersonic crack, the muzzle
 * blast's body and low thump, a rough tail and the slap off the mountains. */
#define SND_RATE 48000
#define SND_LEN  (SND_RATE * 13 / 10)
static int16_t *s_snd;                  /* stereo */
static uint32_t s_snd_done;             /* shots started (audio thread) */
static int s_voice[4] = {-1, -1, -1, -1};

static void snd_build(void)
{
    float *L = (float *)calloc(SND_LEN, sizeof(float)), *R = (float *)calloc(SND_LEN, sizeof(float));
    float lp1 = 0, lp2 = 0, tl = 0, tr = 0, hp_prev = 0, peak = 0, ph = 0;
    uint32_t seed = 12345u;
    int i;
    if (!L || !R) { free(L); free(R); return; }
    for (i = 0; i < SND_LEN; i++) {
        float t = (float)i / SND_RATE, n1, n2, n3, crack, body, thump, tail, fr;
        seed = seed * 1664525u + 1013904223u; n1 = (float)(seed >> 8) / 8388608.0f - 1.0f;
        seed = seed * 1664525u + 1013904223u; n2 = (float)(seed >> 8) / 8388608.0f - 1.0f;
        seed = seed * 1664525u + 1013904223u; n3 = (float)(seed >> 8) / 8388608.0f - 1.0f;
        crack = (n1 - hp_prev) * expf(-t / 0.0025f) * 0.9f;   /* bright, very short */
        hp_prev = n1;
        lp1 += (n1 - lp1) * 0.18f;
        body = lp1 * 2.4f * (t < 0.0006f ? t / 0.0006f : expf(-(t - 0.0006f) / 0.03f));
        fr = 55.0f + 110.0f * expf(-t / 0.025f);
        ph += 6.2831853f * fr / SND_RATE;
        thump = sinf(ph) * expf(-t / 0.07f) * 0.75f;
        lp2 += (n2 - lp2) * 0.05f;
        tail = lp2 * 1.8f * (t < 0.01f ? 0 : expf(-(t - 0.01f) / 0.28f)) * 0.5f;
        tl += (n3 - tl) * 0.04f;
        tr += (-n3 * 0.7f + n2 * 0.3f - tr) * 0.04f;
        L[i] = crack + body + thump + tail + tl * 0.25f * expf(-t / 0.4f);
        R[i] = crack + body + thump + tail + tr * 0.25f * expf(-t / 0.4f);
    }
    /* mountain echoes: two dull, delayed copies */
    for (i = SND_LEN - 1; i >= 0; i--) {
        int d1 = i - (int)(0.31f * SND_RATE), d2 = i - (int)(0.58f * SND_RATE);
        if (d1 >= 0) { L[i] += L[d1] * 0.16f; R[i] += R[d1] * 0.20f; }
        if (d2 >= 0) { L[i] += L[d2] * 0.09f; R[i] += R[d2] * 0.07f; }
    }
    for (i = 0; i < SND_LEN; i++) {
        if (fabsf(L[i]) > peak) peak = fabsf(L[i]);
        if (fabsf(R[i]) > peak) peak = fabsf(R[i]);
    }
    s_snd = (int16_t *)malloc((size_t)SND_LEN * 2 * sizeof(int16_t));
    if (s_snd && peak > 0)
        for (i = 0; i < SND_LEN; i++) {
            float fade = i > SND_LEN - 2400 ? (float)(SND_LEN - i) / 2400.0f : 1.0f;
            s_snd[2 * i] = (int16_t)(L[i] / peak * 30000.0f * fade);
            s_snd[2 * i + 1] = (int16_t)(R[i] / peak * 30000.0f * fade);
        }
    free(L); free(R);
}

/* Audio thread: each 48 kHz stereo frame, before it goes out. */
static void snd_mix(int16_t *st, int n)
{
    uint32_t req = s_snd_req;
    int i, k;
    if (!s_snd) return;
    while (s_snd_done != req) {                  /* start new shots */
        int best = 0;
        for (k = 0; k < 4; k++)
            if (s_voice[k] < 0 || s_voice[k] > s_voice[best]) best = k;
        for (k = 0; k < 4; k++)
            if (s_voice[k] < 0) { best = k; break; }
        s_voice[best] = 0;
        s_snd_done++;
    }
    for (k = 0; k < 4; k++) {
        if (s_voice[k] < 0) continue;
        for (i = 0; i < n && s_voice[k] < SND_LEN; i++, s_voice[k]++) {
            int l = st[2 * i] + (s_snd[2 * s_voice[k]] * 3) / 4;
            int r = st[2 * i + 1] + (s_snd[2 * s_voice[k] + 1] * 3) / 4;
            st[2 * i] = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
            st[2 * i + 1] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
        }
        if (s_voice[k] >= SND_LEN) s_voice[k] = -1;
    }
}

#ifdef _WIN32
extern void (*xbox_ApuPostMix)(int16_t *stereo, int samples);
#define HAVE_POSTMIX() 1
#else
extern void (*xbox_ApuPostMix)(int16_t *stereo, int samples) __attribute__((weak));
#define HAVE_POSTMIX() (&xbox_ApuPostMix != NULL)
#endif

static void snd_init(void)
{
    snd_build();
    if (s_snd && HAVE_POSTMIX())
        xbox_ApuPostMix = snd_mix;
}
