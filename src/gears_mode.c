/*
 * Stick-shift snowboarding (RECOMP_GEARS=1; F5 toggles it in a race). Just
 * for fun: the board has an 18-speed Eaton Fuller truck gearbox.
 *
 * The box, as on the real thing:
 *   - H pattern of three columns: R / LL, 1 / 2, 3 / 4. With the range
 *     selector up (high) the 1-4 positions are 5-8. LL works in either range.
 *   - Splitter: every position has a low and a high split. 2 LL + 8 low +
 *     8 high = 18 forward gears.
 *   - Range and splitter are preselected: the range engages when the stick
 *     passes through neutral, the splitter when the clutch is down (or in
 *     neutral).
 * Controls: hold G and move the mouse to move the stick (it follows the
 * gate: across only on the neutral rail, in and out only in a column),
 * Space = clutch, R = range selector, F = splitter.
 *
 * The engine: each gear has a top speed (redline), from 400 in LL-L to
 * 3600 in 8H (geometric); rpm = board speed / that. In gear with the clutch
 * up it
 *   - pulls between 0.45 and redline (0.3 at the bottom, peak at 0.85),
 *   - drags the board back below 0.45 (bogging: a too-high gear is slow),
 *   - holds the board at redline (engine braking: steep slope, shift up),
 *   - stalls below 0.15, and lugs to a stall below 0.30 for 1.5 s.
 *   LL to 1H can slip the clutch from rest.
 * The board resists: only 20% of the speed the game's physics adds on the
 * ground is kept (RECOMP_GEARS_GRAVITY) and rolling + air drag is taken off
 * (RECOMP_GEARS_DRAG), so coasting tops out around 900 and the engine does
 * the pushing; slowing down is the game's own.
 * The other riders get the same box with an automatic driver: each shift
 * is 0.35-0.75 s of coasting in neutral, and 4% of them grind (wipeout).
 * A missed shift -- into gear without the clutch and with the rpm off by
 * more than 0.25 (the revs hang in neutral, so a quick shift floats in), a downshift that would spin past 1.25 x redline, reverse
 * at speed, or a stall -- grinds the box and throws the rider over the
 * nose (the game's own wipeout, via fps_mode.c).
 *
 * Writes the rider's velocity (+0x250) after the game's update.
 *   RECOMP_GEARS=1           start with it on (F5 toggles)
 *   RECOMP_GEARS_POWER=<f>   engine pull (default 1)
 *   RECOMP_GEARS_SENS=<f>    stick speed (default 1)
 *   RECOMP_GEARS_TOP=<8H>    or <LL-L>,<8H>: gear top speeds (400,3600)
 *   RECOMP_GEARS_GRAVITY=<f> share of the slope's own speed-up kept (0.2)
 *   RECOMP_GEARS_DRAG=<f>    board drag factor (1)
 *   RECOMP_GEARS_PROBE=1     where the velocity lives (see probe_scan)
 *   RECOMP_GEARS_AI=0        leave the other riders alone
 *   RECOMP_GEARS_AI_MISS=<%> their missed-shift rate (default 4)
 * Calibration: [GEARS] logs the player's ground speed (p50/p90/max) every
 * 15 s; LL-L..8H should span the slowest to the fastest of it.
 * [GEARS] logs every shift and failure.
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

#define NGEARS      18
#define GEAR_R      18
#define IN_GEAR_Y   0.8f
#define FLOAT_TOL   0.25f      /* rpm mismatch a clutchless shift gets away with */
#define VK_G        'G'
#define VK_R        'R'
#define VK_F        'F'
#define VK_SPACE_   0x20
#define VK_F5_      0x74

extern uint32_t xbox_TickCountMs(void);
extern int ssx3_HostKey(int vk);
extern void ssx3_GearsMouse(int *dx, int *dy);
extern uint32_t ssx3_PlayerRider(int *mode);
extern void ssx3_RiderWipeout(uint32_t b, const float dir[3]);
extern uint32_t ssx3_RiderN(int i, int *mode);

static int s_inited, s_enabled;
static float s_power = 1.0f, s_sens = 1.0f;
static float s_top0 = 400.0f, s_top8 = 3600.0f;     /* LL-L / 8H top speeds */
static float s_ai_miss = 0.04f;                     /* AI: share of shifts that grind */
static float s_gravity = 0.2f;                      /* share of the game's own speed-up kept */
static float s_drag = 1.0f;                         /* board drag factor */
static float s_prev_speed;                          /* player: speed we left the board at */
static int s_ai_on = 1;
static uint32_t s_live, s_last_tick, s_lug_ms, s_dead_until;
static int s_range_hi, s_split_hi;          /* preselected */
static int s_eng_range, s_eng_split;        /* engaged */
static int s_r_was, s_f_was, s_f5_was;

/* Read by the flip thread. */
static volatile int s_hud;                  /* in a race with the mode on */
static volatile float s_kx, s_ky;           /* stick in gate units */
static volatile int s_gear = -1;            /* engaged: -1 neutral, 0..17, GEAR_R */
static volatile float s_rpm, s_grade;
static volatile int s_clutch, s_hud_range, s_hud_split, s_hud_eng_range;
static volatile uint32_t s_fail_t;
static const char *volatile s_fail_msg, *volatile s_fail_hint;

static float rdf(uint32_t va) { return MEMF(va); }
static void wrf(uint32_t va, float v) { MEMF(va) = v; }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

static float gear_top(int g)
{
    return s_top0 * powf(s_top8 / s_top0, (float)g / 17.0f);
}

static void gear_name(int g, char *buf, size_t n)
{
    if (g < 0)
        snprintf(buf, n, "N");
    else if (g == GEAR_R)
        snprintf(buf, n, "R");
    else if (g < 2)
        snprintf(buf, n, "LL%c", g ? 'H' : 'L');
    else
        snprintf(buf, n, "%d%c", (g - 2) / 2 + 1, (g - 2) & 1 ? 'H' : 'L');
}

/* The gear the stick, range and splitter select (-1 = neutral). */
static int knob_gear(void)
{
    int col, top, pos;
    if (fabsf(s_ky) < IN_GEAR_Y)
        return -1;
    col = (int)lroundf(s_kx);
    top = s_ky < 0;
    if (col < 0)
        return top ? GEAR_R : s_eng_split;
    pos = (col == 0 ? 1 : 3) + (top ? 0 : 1) + (s_eng_range ? 4 : 0);
    return 2 + (pos - 1) * 2 + s_eng_split;
}

/* Move the stick through the gate: across only on the neutral rail, in and
 * out only in a column. Mouse right/down = stick right/down. */
static void move_knob(float mx, float my)
{
    float kx = s_kx, ky = s_ky, col = clampf(roundf(kx), -1, 1);
    if (fabsf(ky) > 0.1f) {
        kx = col;
        ky = clampf(ky + my, -1, 1);
        if (fabsf(ky) <= 0.1f) {            /* back on the rail: may go across */
            ky = 0;
            kx = clampf(kx + mx, -1, 1);
        }
    } else {
        kx = clampf(kx + mx, -1, 1);
        col = clampf(roundf(kx), -1, 1);
        if (fabsf(kx - col) < 0.15f && fabsf(my) > 0.0f) {
            kx = col;
            ky = clampf(my, -1, 1);
        } else {
            ky = 0;
        }
    }
    s_kx = kx;
    s_ky = ky;
}

static void reset_box(void)
{
    s_kx = s_ky = 0;
    s_gear = -1;
    s_rpm = 0.12f;
    s_lug_ms = 0;
    s_prev_speed = 0;
    s_range_hi = s_eng_range = 0;
    s_split_hi = s_eng_split = 0;
}

/* A missed shift: grind, stall, over the nose. */
static void fail(uint32_t me, const char *why, const char *hint, const float vel[3], uint32_t now)
{
    float d[3] = {vel[0], vel[1], 0}, l = sqrtf(vel[0] * vel[0] + vel[1] * vel[1]);
    char name[8];
    if (l < 1.0f) { d[0] = 1; d[1] = 0; l = 1; }
    d[0] /= l; d[1] /= l;
    gear_name(s_gear, name, sizeof name);
    fprintf(stderr, "[GEARS] t=%u %s in %s (rpm %.2f, speed %.0f)\n",
            now, why, name, s_rpm, sqrtf(vel[0] * vel[0] + vel[1] * vel[1] + vel[2] * vel[2]));
    ssx3_RiderWipeout(me, d);
    s_fail_msg = why;
    s_fail_hint = hint;
    s_fail_t = now;
    s_kx = s_ky = 0;
    s_gear = -1;
    s_rpm = 0;
    s_lug_ms = 0;
    s_prev_speed = 0;
    s_dead_until = now + 1500;
}

/* Whether the gearbox takes the mouse this update (fps_mode.c asks). */
int ssx3_GearsWantMouse(void)
{
    return s_hud && ssx3_HostKey(VK_G);
}

#define BOG_RPM     0.45f      /* below: the engine drags the board back */
#define LUG_RPM     0.30f      /* below for LUG_MS: stalls (player) */
#define LUG_MS      1500u
#define STALL_RPM   0.15f

/* Scale the rider's velocity by k: rider +0x250 and the two copies the
 * physics object [+0x74C] keeps at +0xC30 / +0xC70 (the probe found all
 * three the same length every update). */
static void scale_vel(uint32_t b, float vel[3], float k)
{
    uint32_t phys = MEM32(b + 0x74C), o;
    int i;
    vel[0] *= k; vel[1] *= k; vel[2] *= k;
    wrf(b + 0x250, vel[0]);
    wrf(b + 0x254, vel[1]);
    wrf(b + 0x258, vel[2]);
    for (o = 0xC30; o <= 0xC70; o += 0x40)
        for (i = 0; i < 3; i++)
            wrf(phys + o + 4u * i, rdf(phys + o + 4u * i) * k);
}

/* Power curve over the band BOG_RPM .. redline: 0.3 at the bottom, peak
 * at 0.85 -- the gear that puts the rpm near 0.85 pulls three times
 * harder than one at the bottom of its band. */
static float power_curve(float r)
{
    if (r < 0.85f) return 0.3f + 0.7f * (r - BOG_RPM) / (0.85f - BOG_RPM);
    return clampf(1.0f - (r - 0.85f) / 0.15f * 0.5f, 0, 1);
}

/* The engine on rider b in gear g, clutch up, on the ground: pulls in its
 * band (power / speed, at most ACCEL_MAX), bogs (drags) below it, holds
 * the board at redline. LL to 1H slip the clutch from rest. Returns the
 * rpm (fraction of redline). */
#define ENGINE_P    8.0e6f
#define ACCEL_MAX   1200.0f

static float engine(uint32_t b, float vel[3], float speed, int g, float dt)
{
    float rpm = speed / gear_top(g), k = 1.0f, a;
    if (rpm > 1.0f) {
        k = 1.0f / rpm;
    } else if (speed > 30.0f) {
        if (g <= 3 && rpm < 0.3f)
            a = s_power * ENGINE_P * 0.8f / (speed > 400.0f ? speed : 400.0f);
        else if (rpm < BOG_RPM)
            a = -s_power * 1500.0f * (BOG_RPM - rpm) / BOG_RPM;
        else
            a = s_power * ENGINE_P * power_curve(rpm) / (speed > 400.0f ? speed : 400.0f);
        if (a > ACCEL_MAX * s_power) a = ACCEL_MAX * s_power;
        k = 1.0f + a * dt / speed;
        if (k < 0.0f) k = 0.0f;
    }
    if (k != 1.0f)
        scale_vel(b, vel, k);
    return rpm;
}

/* The board's own resistance, on the ground: of the speed the game's
 * physics added since our last update (gravity down the slope) only
 * s_gravity is kept, and rolling + air drag (DRAG_ROLL + DRAG_AIR v^2,
 * times s_drag) is taken off -- so coasting tops out at ~900 (about
 * 30 km/h if the units are cm/s) on a run where the game alone reaches
 * 2000+, and the engine has to push the board. Slowing down (carving,
 * braking, uphill) is the game's own. prev = the speed we left it at,
 * 0 = none (first update on the ground). Returns the new speed. */
#define DRAG_ROLL   50.0f
#define DRAG_AIR    3.1e-4f

static float board_drag(uint32_t b, float vel[3], float speed, float prev, float dt)
{
    float want = speed;
    if (prev > 0.0f && speed > prev)
        want = prev + (speed - prev) * s_gravity;
    want -= s_drag * (DRAG_ROLL + DRAG_AIR * want * want) * dt;
    if (want < 0.0f)
        want = 0.0f;
    if (speed > 1.0f && want != speed)
        scale_vel(b, vel, want / speed);
    return want;
}

static float vlen(const float v[3]) { return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

/* ---- the other riders: the same box, driven by an automatic driver. A
 * shift is 0.35-0.75 s in neutral (no pull, no governor) and grinds --
 * wipeout -- s_ai_miss of the time. */

typedef struct { uint32_t b, shift_until, seed; int gear, pending; float prev; } AiBox;
static AiBox s_ai[6];

static float ai_rand(AiBox *a)
{
    uint32_t x = (a->seed += 0x9E3779B9u);
    x ^= x >> 16; x *= 0x7FEB352Du; x ^= x >> 15; x *= 0x846CA68Bu; x ^= x >> 16;
    return (float)(x >> 8) / 16777216.0f;
}

/* The lowest gear that keeps the rpm at or under 0.88. */
static int ai_pick(float speed)
{
    int g;
    for (g = 0; g < NGEARS - 1; g++)
        if (speed / gear_top(g) <= 0.88f)
            return g;
    return NGEARS - 1;
}

static void ai_tick(int i, uint32_t now, float dt)
{
    AiBox *a = &s_ai[i];
    float vel[3], speed, rpm;
    int mode;
    uint32_t b = ssx3_RiderN(i, &mode);

    if (!b || mode == 3 || mode == 12) {
        a->b = 0;
        return;
    }
    if (a->b != b) {
        memset(a, 0, sizeof *a);
        a->b = b;
        a->seed = b ^ (uint32_t)i * 0x85EBCA6Bu ^ now;
    }
    if (mode == 2 || mode == 14 || mode == 15) {
        a->gear = -1;                       /* down: back in a fitting gear after */
        a->shift_until = 0;
        a->prev = 0;
        return;
    }
    if (mode == 1 || mode == 10 || mode == 11) {
        a->prev = 0;
        return;
    }
    vel[0] = rdf(b + 0x250); vel[1] = rdf(b + 0x254); vel[2] = rdf(b + 0x258);
    if (!isfinite(vel[0]) || !isfinite(vel[1]) || !isfinite(vel[2]))
        return;
    speed = board_drag(b, vel, vlen(vel), a->prev, dt);
    a->prev = speed;
    if (a->gear < 0)
        a->gear = ai_pick(speed);
    if (a->shift_until) {
        if ((int32_t)(now - a->shift_until) < 0)
            return;                         /* in neutral, coasting */
        a->shift_until = 0;
        if (ai_rand(a) < s_ai_miss) {
            float d[3] = {vel[0], vel[1], 0}, l = sqrtf(vel[0] * vel[0] + vel[1] * vel[1]);
            if (l < 1.0f) { d[0] = 1; l = 1; }
            d[0] /= l; d[1] /= l;
            fprintf(stderr, "[GEARS] t=%u rider %d grinds (%d -> %d, speed %.0f)\n",
                    now, i, a->gear, a->pending, speed);
            ssx3_RiderWipeout(b, d);
            a->gear = -1;
            a->prev = 0;
            return;
        }
        a->gear = a->pending;
    }
    rpm = speed / gear_top(a->gear);
    if (rpm > 0.97f || (rpm < 0.6f && a->gear > 0)) {
        int p = ai_pick(speed);
        if (p != a->gear) {
            a->pending = p;
            a->shift_until = now + 350u + (uint32_t)(ai_rand(a) * 400.0f);
            return;
        }
    }
    engine(b, vel, speed, a->gear, dt);
    a->prev = vlen(vel);
}


/* ---- RECOMP_GEARS_PROBE=1: does the velocity we write stick, and where
 * else does the game keep it? Each ground update it compares the speed it
 * finds with the one it left; and scans the rider (0..0x900) and its
 * physics object [+0x74C] (0..0x1000) for 16-byte aligned float triples
 * whose length matches the rider's speed (within 3%). Every 5 s it logs
 * the persistence ratio and the best-matching offsets. */

static int s_probe;
static double s_probe_ratio;
static uint32_t s_probe_nr, s_probe_t, s_probe_ticks;
static uint32_t s_probe_hit[2][0x100];

static void probe_scan(uint32_t me, float speed, uint32_t now)
{
    uint32_t phys = MEM32(me + 0x74C), base[2] = {me, phys}, lim[2] = {0x900, 0x1000};
    int k, i, j;
    if (speed < 300.0f)
        return;
    if (s_prev_speed > 0.0f) {              /* the speed we left it at */
        s_probe_ratio += speed / s_prev_speed;
        s_probe_nr++;
    }
    s_probe_ticks++;
    for (k = 0; k < 2; k++)
        for (i = 0; (uint32_t)i * 16 < lim[k]; i++) {
            float x = rdf(base[k] + i * 16), y = rdf(base[k] + i * 16 + 4), z = rdf(base[k] + i * 16 + 8);
            float l;
            if (!isfinite(x) || !isfinite(y) || !isfinite(z)) continue;
            l = sqrtf(x * x + y * y + z * z);
            if (fabsf(l - speed) < speed * 0.03f)
                s_probe_hit[k][i]++;
        }
    if (!s_probe_t)
        s_probe_t = now;
    if (now - s_probe_t < 5000 || !s_probe_ticks)
        return;
    fprintf(stderr, "[GEARS-PROBE] speed now %.0f, found/left %.3f over %u updates; matches of %u:",
            speed, s_probe_nr ? s_probe_ratio / s_probe_nr : 0.0, s_probe_nr, s_probe_ticks);
    for (j = 0; j < 10; j++) {
        int bk = -1, bi = 0;
        uint32_t best = 0;
        for (k = 0; k < 2; k++)
            for (i = 0; (uint32_t)i * 16 < lim[k]; i++)
                if (s_probe_hit[k][i] > best) { best = s_probe_hit[k][i]; bk = k; bi = i; }
        if (bk < 0 || best * 2 < s_probe_ticks) break;
        fprintf(stderr, " %s+0x%X=%u", bk ? "phys" : "rider", bi * 16, best);
        s_probe_hit[bk][bi] = 0;
    }
    fprintf(stderr, "\n");
    memset(s_probe_hit, 0, sizeof s_probe_hit);
    s_probe_ratio = 0;
    s_probe_nr = s_probe_ticks = 0;
    s_probe_t = now;
}

/* ---- calibration: the player's ground speed, every 15 s */

static uint32_t s_speed_hist[100], s_speed_n, s_speed_t;

static void speed_note(float speed, uint32_t now)
{
    int k = (int)(speed / 100.0f), i;
    uint32_t acc = 0, p50 = 0, p90 = 0, mx = 0;
    if (k < 0) k = 0;
    if (k > 99) k = 99;
    s_speed_hist[k]++;
    s_speed_n++;
    if (!s_speed_t)
        s_speed_t = now;
    if (now - s_speed_t < 15000)
        return;
    for (i = 0; i < 100; i++) {
        acc += s_speed_hist[i];
        if (!p50 && acc * 2 >= s_speed_n) p50 = (uint32_t)i * 100 + 50;
        if (!p90 && acc * 10 >= s_speed_n * 9) p90 = (uint32_t)i * 100 + 50;
        if (s_speed_hist[i]) mx = (uint32_t)i * 100 + 50;
    }
    fprintf(stderr, "[GEARS] ground speed last 15 s: p50 %u p90 %u max %u (gear tops %.0f..%.0f)\n",
            p50, p90, mx, s_top0, s_top8);
    memset(s_speed_hist, 0, sizeof s_speed_hist);
    s_speed_n = 0;
    s_speed_t = now;
}

/* Called on the game thread after every in-game update (sub_000CC8F0),
 * after the FPS mode's tick (which polls the mouse). */
void ssx3_GearsTick(void)
{
    uint32_t me, now = xbox_TickCountMs();
    float vel[3], speed, speed_h, dt, road;
    int mode, f5, r, f, clutch, g, air, wipe, dx, dy;
    const char *e;

    if (!s_inited) {
        s_inited = 1;
        e = getenv("RECOMP_GEARS");
        s_enabled = e && *e && *e != '0';
        if ((e = getenv("RECOMP_GEARS_POWER")) && atof(e) >= 0) s_power = (float)atof(e);
        if ((e = getenv("RECOMP_GEARS_SENS")) && atof(e) > 0) s_sens = (float)atof(e);
        if ((e = getenv("RECOMP_GEARS_TOP")) && atof(e) > 0) {
            const char *c = strchr(e, ',');
            if (c) {                        /* <LL-L>,<8H> */
                s_top0 = (float)atof(e);
                if (atof(c + 1) > s_top0) s_top8 = (float)atof(c + 1);
            } else {
                s_top8 = (float)atof(e);
            }
        }
        if ((e = getenv("RECOMP_GEARS_AI"))) s_ai_on = *e != '0';
        s_probe = getenv("RECOMP_GEARS_PROBE") != NULL;
        if ((e = getenv("RECOMP_GEARS_DRAG")) && atof(e) >= 0) s_drag = (float)atof(e);
        if ((e = getenv("RECOMP_GEARS_GRAVITY"))) s_gravity = clampf((float)atof(e), 0, 1);
        if ((e = getenv("RECOMP_GEARS_AI_MISS"))) s_ai_miss = (float)atof(e) / 100.0f;
        reset_box();
        if (s_enabled)
            fprintf(stderr, "[GEARS] 18-speed gearbox on (F5 toggles)\n");
    }
    f5 = ssx3_HostKey(VK_F5_);
    if (f5 && !s_f5_was) {
        s_enabled = !s_enabled;
        fprintf(stderr, "[GEARS] %s\n", s_enabled ? "on" : "off");
    }
    s_f5_was = f5;

    dt = s_last_tick && now > s_last_tick ? (float)(now - s_last_tick) / 1000.0f : 0.016f;
    if (dt > 0.1f) dt = 0.1f;
    s_last_tick = now;

    me = s_enabled ? ssx3_PlayerRider(&mode) : 0;
    if (me) {
        /* only once the race is on: Static (3) / Gate (12) until the start */
        if (mode == 12)
            s_live = 0;
        else if (mode != 3)
            s_live = me;
    }
    if (!me || s_live != me) {
        if (s_hud)
            reset_box();
        s_hud = 0;
        s_live = me ? s_live : 0;
        return;
    }
    s_hud = 1;
    if (s_ai_on) {
        int i;
        for (i = 1; i < 6; i++)
            ai_tick(i, now, dt);
    }

    clutch = ssx3_HostKey(VK_SPACE_);
    r = ssx3_HostKey(VK_R);
    f = ssx3_HostKey(VK_F);
    if (r && !s_r_was) s_range_hi = !s_range_hi;
    if (f && !s_f_was) s_split_hi = !s_split_hi;
    s_r_was = r;
    s_f_was = f;
    ssx3_GearsMouse(&dx, &dy);
    if (dx || dy)
        move_knob((float)dx / 120.0f * s_sens, (float)dy / 120.0f * s_sens);

    vel[0] = rdf(me + 0x250); vel[1] = rdf(me + 0x254); vel[2] = rdf(me + 0x258);
    if (!isfinite(vel[0]) || !isfinite(vel[1]) || !isfinite(vel[2]))
        return;
    air = mode == 1 || mode == 10 || mode == 11;
    wipe = mode == 2 || mode == 14 || mode == 15;
    if (s_probe && !air && !wipe)
        probe_scan(me, vlen(vel), now);
    if (air || wipe)
        s_prev_speed = 0;
    else
        s_prev_speed = board_drag(me, vel, vlen(vel), s_prev_speed, dt);
    speed = vlen(vel);
    speed_h = sqrtf(vel[0] * vel[0] + vel[1] * vel[1]);
    if (!air && !wipe && speed_h > 100.0f)
        s_grade = s_grade + (-vel[2] / speed_h * 100.0f - s_grade) * 0.1f;
    if (!air && !wipe)
        speed_note(speed, now);

    /* preselected range / splitter engage */
    if (fabsf(s_ky) < IN_GEAR_Y)
        s_eng_range = s_range_hi;
    if (clutch || fabsf(s_ky) < IN_GEAR_Y)
        s_eng_split = s_split_hi;
    s_clutch = clutch;
    s_hud_range = s_range_hi;
    s_hud_split = s_split_hi;
    s_hud_eng_range = s_eng_range;

    if (wipe) {                             /* down: nothing to drive */
        s_lug_ms = 0;
        return;
    }

    g = knob_gear();
    road = g >= 0 && g < NGEARS ? speed / gear_top(g) : 0.0f;
    if (g != s_gear) {
        char a[8], b[8];
        gear_name(s_gear, a, sizeof a);
        gear_name(g, b, sizeof b);
        if (g >= 0 && now < s_dead_until) {
            /* engine still dead after a failure: nothing to grind */
        } else if (g == GEAR_R && speed > 150.0f) {
            fail(me, "REVERSE AT SPEED", "STOP BEFORE REVERSE", vel, now);
            return;
        } else if (g >= 0 && g < NGEARS && road > 1.25f) {
            fail(me, "OVER-REV", "TOO LOW A GEAR FOR THIS SPEED", vel, now);
            return;
        } else if (g >= 0 && g < NGEARS && !clutch && fabsf(road - s_rpm) > FLOAT_TOL) {
            fail(me, "GRIND", "HOLD SPACE - THE CLUTCH - WHILE YOU SHIFT", vel, now);
            return;
        }
        fprintf(stderr, "[GEARS] t=%u %s -> %s (rpm %.2f, clutch %d, speed %.0f, grade %.0f%%)\n",
                now, a, b, g >= 0 && g < NGEARS ? road : s_rpm, clutch, speed, s_grade);
        s_gear = g;
    }

    if (s_gear >= 0 && s_gear < NGEARS && !clutch) {
        float rpm = road;
        if (!air) {
            if (s_gear > 3 && rpm < STALL_RPM) {
                fail(me, "STALL", "TOO HIGH A GEAR - SHIFT DOWN", vel, now);
                return;
            }
            if (s_gear > 3 && rpm < LUG_RPM) {
                s_lug_ms += (uint32_t)(dt * 1000.0f);
                if (s_lug_ms > LUG_MS) {
                    fail(me, "LUGGED IT", "TOO HIGH A GEAR - SHIFT DOWN", vel, now);
                    return;
                }
            } else {
                s_lug_ms = 0;
            }
            engine(me, vel, speed, s_gear, dt);
            s_prev_speed = vlen(vel);
        }
        s_rpm = rpm;
    } else {
        /* neutral or clutch down: the engine falls back to idle, slowly
         * enough that a shift through neutral can still float in */
        float idle = now < s_dead_until ? 0.0f : 0.12f;
        s_rpm += (idle - s_rpm) * (1.0f - expf(-dt / 5.0f));
        s_lug_ms = 0;
    }
}

/* =================================================================== *
 * HUD: gate with the stick, range / splitter, gear, tach, grade, clutch
 * lamp; a failure in big letters. Drawn into each flipped frame by the
 * Vulkan backend's flip thread (after the FPS mode's overlay).
 * =================================================================== */

static uint32_t *s_px;
static int s_w, s_h, s_stride;

static const unsigned char s_font[][7] = {
    {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}, {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},   /* A B */
    {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E},   /* C D */
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},   /* E F */
    {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}, {0x11,0x11,0x11,0x1F,0x11,0x11,0x11},   /* G H */
    {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}, {0x07,0x02,0x02,0x02,0x02,0x12,0x0C},   /* I J */
    {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, {0x10,0x10,0x10,0x10,0x10,0x10,0x1F},   /* K L */
    {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}, {0x11,0x11,0x19,0x15,0x13,0x11,0x11},   /* M N */
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},   /* O P */
    {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}, {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},   /* Q R */
    {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}, {0x1F,0x04,0x04,0x04,0x04,0x04,0x04},   /* S T */
    {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}, {0x11,0x11,0x11,0x11,0x11,0x0A,0x04},   /* U V */
    {0x11,0x11,0x11,0x15,0x15,0x15,0x0A}, {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},   /* W X */
    {0x11,0x11,0x11,0x0A,0x04,0x04,0x04}, {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}};  /* Y Z */
static const unsigned char s_digit[10][7] = {
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
    {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E},
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}};

static const unsigned char *font_glyph(char c)
{
    static const unsigned char pct[7] = {0x18,0x19,0x02,0x04,0x08,0x13,0x03};
    static const unsigned char bang[7] = {0x04,0x04,0x04,0x04,0x04,0x00,0x04};
    static const unsigned char dash[7] = {0x00,0x00,0x00,0x1F,0x00,0x00,0x00};
    if (c >= 'A' && c <= 'Z') return s_font[c - 'A'];
    if (c >= '0' && c <= '9') return s_digit[c - '0'];
    if (c == '%') return pct;
    if (c == '!') return bang;
    if (c == '-') return dash;
    return NULL;
}

static void blend_rect(int x0, int y0, int x1, int y1, uint32_t rgb, float a)
{
    int x, y;
    float r = (float)((rgb >> 16) & 255), g = (float)((rgb >> 8) & 255), b = (float)(rgb & 255);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > s_w) x1 = s_w;
    if (y1 > s_h) y1 = s_h;
    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++) {
            uint32_t *d = &s_px[(size_t)y * s_stride + x], o = *d;
            *d = 0xFF000000u
                 | ((uint32_t)(((o >> 16) & 255) * (1 - a) + r * a) << 16)
                 | ((uint32_t)(((o >> 8) & 255) * (1 - a) + g * a) << 8)
                 | (uint32_t)((o & 255) * (1 - a) + b * a);
        }
}

static void fill_rect(int x0, int y0, int x1, int y1, uint32_t rgb)
{
    blend_rect(x0, y0, x1, y1, rgb, 1.0f);
}

static void disc(float cx, float cy, float rad, uint32_t rgb)
{
    int x, y;
    for (y = (int)(cy - rad); y <= (int)(cy + rad); y++)
        for (x = (int)(cx - rad); x <= (int)(cx + rad); x++) {
            float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy;
            float a = clampf(rad - sqrtf(dx * dx + dy * dy), 0, 1);
            if (a > 0 && x >= 0 && y >= 0 && x < s_w && y < s_h)
                blend_rect(x, y, x + 1, y + 1, rgb, a);
        }
}

static int text_w(const char *s, int sc) { return (int)strlen(s) * 6 * sc - sc; }

static void text(const char *s, int x, int y, int sc, uint32_t rgb)
{
    int i, r, k, o;
    for (o = 0; o < 2; o++)
        for (i = 0; s[i]; i++) {
            const unsigned char *g = font_glyph(s[i]);
            int gx = x + i * 6 * sc + (o ? 0 : (sc + 1) / 2), gy = y + (o ? 0 : (sc + 1) / 2);
            if (!g) continue;
            for (r = 0; r < 7; r++)
                for (k = 0; k < 5; k++)
                    if (g[r] & (0x10 >> k))
                        fill_rect(gx + k * sc, gy + r * sc, gx + (k + 1) * sc, gy + (r + 1) * sc,
                                  o ? rgb : 0x000000u);
        }
}

void ssx3_GearsOverlay(uint32_t *px, int w, int h, int stride)
{
    uint32_t now = xbox_TickCountMs(), ft = s_fail_t;
    const char *fmsg = s_fail_msg, *fhint = s_fail_hint;
    int gs, big, pw, ph, x0, y0, gx, gy, cw, rh, c, i;
    float u, rpm = s_rpm;
    char buf[16];
    static const char *const labels[2][6] = {
        {"R", "LL", "1", "2", "3", "4"}, {"R", "LL", "5", "6", "7", "8"}};

    if (!s_hud || !px || w < 160 || h < 120)
        return;
    s_px = px; s_w = w; s_h = h; s_stride = stride;
    u = (float)h / 480.0f;
    gs = h / 400 + 1;
    big = h / 110 + 1;

    /* panel, bottom left of the 4:3 middle */
    pw = (int)(190 * u);
    ph = (int)(132 * u);
    x0 = w / 2 - (int)(310 * u);
    y0 = h - ph - (int)(14 * u);
    blend_rect(x0, y0, x0 + pw, y0 + ph, 0x0A0A0Cu, 0.6f);

    /* the gate */
    cw = (int)(26 * u);
    rh = (int)(28 * u);
    gx = x0 + (int)(18 * u) + cw;
    gy = y0 + (int)(14 * u) + rh;
    fill_rect(gx - cw, gy - 1, gx + cw + 1, gy + 2, 0x707070u);
    for (c = -1; c <= 1; c++) {
        fill_rect(gx + c * cw - 1, gy - rh, gx + c * cw + 2, gy + rh, 0x707070u);
        for (i = 0; i < 2; i++) {
            const char *l = labels[s_hud_eng_range][(c + 1) * 2 + i];
            text(l, gx + c * cw - text_w(l, gs) / 2, i ? gy + rh + 2 * gs : gy - rh - 9 * gs,
                 gs, 0xD0D0D0u);
        }
    }
    disc((float)gx + s_kx * (float)cw, (float)gy + s_ky * (float)rh, 5.0f * u,
         s_gear >= 0 ? 0xF0C040u : 0xE0E0E0u);

    /* gear, range, splitter, clutch */
    {
        int tx = gx + cw + (int)(22 * u), ty = y0 + (int)(12 * u);
        if (s_gear >= 0) {
            int g = s_gear;
            if (g == 18) snprintf(buf, sizeof buf, "R");
            else if (g < 2) snprintf(buf, sizeof buf, "LL%c", g ? 'H' : 'L');
            else snprintf(buf, sizeof buf, "%d%c", (g - 2) / 2 + 1, (g - 2) & 1 ? 'H' : 'L');
        } else {
            snprintf(buf, sizeof buf, "N");
        }
        text(buf, tx, ty, big, 0xFFFFFFu);
        ty += 9 * big;
        text(s_hud_range ? "HI" : "LO", tx, ty, gs + 1,
             s_hud_range == s_hud_eng_range ? 0x60D0FFu : 0x406080u);
        text(s_hud_split ? "SPLIT H" : "SPLIT L", tx + 4 * 6 * (gs + 1), ty, gs, 0xD0D0D0u);
        ty += 10 * (gs + 1);
        text("CLUTCH", tx, ty, gs, s_clutch ? 0x60FF60u : 0x404040u);
        ty += 10 * gs;
        snprintf(buf, sizeof buf, "GRADE %d%%", (int)lroundf(s_grade));
        text(buf, tx, ty, gs, 0xD0D0D0u);
    }

    /* tach: green band, redline */
    {
        int bx = x0 + (int)(12 * u), by = y0 + ph - (int)(18 * u), bw = pw - (int)(24 * u);
        int bh = (int)(8 * u) + 1, fw;
        float r = clampf(rpm / 1.05f, 0, 1);
        fill_rect(bx, by, bx + bw, by + bh, 0x202020u);
        fill_rect(bx + (int)(bw * 0.7f / 1.05f), by + bh - 2, bx + (int)(bw * 0.95f / 1.05f), by + bh, 0x40A040u);
        fill_rect(bx + (int)(bw * 1.0f / 1.05f), by, bx + bw, by + bh, 0x802020u);
        fw = (int)(bw * r);
        fill_rect(bx, by, bx + fw, by + bh - 2,
                  rpm > 1.0f ? 0xFF3030u : rpm > 0.85f ? 0xFFC040u : rpm < BOG_RPM && s_gear > 3 ? 0xFF8030u
                  : 0xF0F0F0u);
        text("RPM", bx, by - 9 * gs, gs, 0xA0A0A0u);
    }

    /* a failure, big */
    if (fmsg && now - ft < 2000) {
        int sc = h / 60 + 1;
        text(fmsg, w / 2 - text_w(fmsg, sc) / 2, h * 30 / 100, sc, 0xFF4030u);
        if (fhint)
            text(fhint, w / 2 - text_w(fhint, gs + 1) / 2, h * 30 / 100 + 9 * sc, gs + 1, 0xFFFFFFu);
    }
}
