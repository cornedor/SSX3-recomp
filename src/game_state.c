/*
 * Game state from the game itself, for the test scripts.
 *
 * SSX 3's app object ([0x003A930C] = 0x00369550) runs a state machine
 * (sub_001AD880): +4 points at the current state object, +8 at a pending next
 * state (0 when none), +0x5C is a phase. A state object's first dword is its
 * vtable, which says what kind of screen it is -- so a screen is identified
 * by that vtable, not by guessing from draw counts.
 *
 * A host thread samples the fields every 100 ms (reads only; a torn read is
 * corrected on the next sample) and
 *   - logs every change to stderr:  [STATE] t=12345 vt=002DE1A0 race next=0 phase=0
 *   - keeps game_state.txt in the working directory current, one line:
 *         <vtable hex> <name> <ms since process start> <seconds in this state>
 *         <vtable hex> <name> <ms since start> <seconds in state> <sub>
 *     which tools\*.ps1 read (tools\game_state.ps1). <sub> is the dword at
 *     state+0xA8: inside "ingame" it changes with the phase (5 at the
 *     briefing, then 8 / 0xC during the race; meaning still being mapped).
 *     <draws> is the GPU executor's total draw count: a frame that never
 *     ends shows as a count that stops moving (tools\to_race.ps1 [freeze]).
 * Names come from the table below; an unknown vtable is reported as "?".
 * RECOMP_GAME_STATE=0 disables it.
 */
#ifdef _WIN32
#include <windows.h>
#else
#include "platform/win32_compat.h"
#endif
#include "recomp/gen/recomp_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define APP_PTR_VA 0x003A930Cu
#define SSX3_RIDERS 8               /* rider slots looked at (a race has 6) */

static const struct { uint32_t vt; const char *name; } k_states[] = {
    /* Learned by watching a run through every screen (tools\to_race.ps1
     * prints the state for each new screen; CLAUDE.md has the table). */
    { 0x0029AD1Cu, "boot" },
    { 0x0029ACD8u, "intro" },        /* EA / intro movies */
    { 0x00289618u, "frontend" },     /* every menu, title to rules */
    { 0x0029AD48u, "event_enter" },  /* brief transition into an event */
    { 0x0029B0DCu, "loading" },      /* race loading screens */
    { 0x00299D7Cu, "ingame" },       /* briefing, race, results, replay */
    { 0, NULL }
};

static const char *state_name(uint32_t vt)
{
    int i;
    for (i = 0; k_states[i].name; i++)
        if (k_states[i].vt == vt)
            return k_states[i].name;
    return "?";
}

static int guest_ok(uint32_t va)
{
    /* Guest heap and image: anything below 64 MB, or the contiguous window. */
    return va >= 0x10000u && (va < 0x04000000u || (va >= 0x80000000u && va < 0x84000000u));
}

/* RECOMP_FE_PROBE=1: find the frontend's "current screen" field. While the
 * state is frontend, the first 0x1000 bytes of the state object are compared
 * with the previous sample (every 300 ms) and each dword that changed between
 * two small values (< 0x400) is logged as [FEPROBE] +off old->new, next to
 * the pad layer's own "PAD: live" press lines -- the field that moves on
 * every screen change and nowhere else is the page. */
#define FE_PROBE_BYTES 0x1000
static void fe_probe(uint32_t cur, uint32_t vt, DWORD t)
{
    static int on = -1;
    static uint32_t prev[FE_PROBE_BYTES / 4], prev_cur;
    uint32_t i;
    if (on < 0) {
        const char *e = getenv("RECOMP_FE_PROBE");
        on = e && e[0] == '1';
    }
    if (!on || vt != 0x00289618u || !guest_ok(cur) || !guest_ok(cur + FE_PROBE_BYTES))
        return;
    static uint32_t prev_vt[FE_PROBE_BYTES / 4];
    for (i = 0; i < FE_PROBE_BYTES / 4; i++) {
        uint32_t v = MEM32(cur + i * 4), pvt = 0;
        if (cur == prev_cur && v != prev[i] && v < 0x400u && prev[i] < 0x400u) {
            fprintf(stderr, "[FEPROBE] t=%lu +%03X %u->%u\n", (unsigned long)t, i * 4, prev[i], v);
        }
        /* Pointer fields: an object whose first dword looks like a vtable
         * (inside the image's data, 0x00280000-0x00340000). A screen object
         * swapped for another shows as its vtable changing. */
        if (guest_ok(v) && v >= 0x80000000u) {
            uint32_t w = MEM32(v);
            if (w >= 0x00280000u && w < 0x00340000u)
                pvt = w;
        }
        if (cur == prev_cur && pvt != prev_vt[i] && (pvt || prev_vt[i]))
            fprintf(stderr, "[FEPROBE] t=%lu +%03X -> obj vt %08X->%08X\n", (unsigned long)t, i * 4,
                    prev_vt[i], pvt);
        prev_vt[i] = pvt;
        prev[i] = v;
    }
    prev_cur = cur;
    fflush(stderr);
}

static DWORD WINAPI game_state_thread(LPVOID unused)
{
    uint32_t last_vt = 0xFFFFFFFFu, last_next = 0xFFFFFFFFu, last_sub = 0xFFFFFFFFu;
    DWORD t0 = GetTickCount(), since = t0;
    char line[160];
    (void)unused;

    for (;;) {
        uint32_t app, cur = 0, next = 0, phase = 0, vt = 0, sub = 0;
        DWORD now;

        Sleep(100);
        now = GetTickCount();
        app = MEM32(APP_PTR_VA);
        if (guest_ok(app)) {
            cur   = MEM32(app + 4);
            next  = MEM32(app + 8);
            phase = MEM32(app + 0x5C);
            if (guest_ok(cur)) {
                vt = MEM32(cur);
                sub = MEM32(cur + 0xA8);   /* in-game sub-state, see below */
            }
        }
        {
            static DWORD last_probe;
            if (now - last_probe >= 300) {
                last_probe = now;
                fe_probe(cur, vt, now - t0);
            }
        }
        if (vt != last_vt || next != last_next || sub != last_sub) {
            if (vt != last_vt || sub != last_sub)
                since = now;
            fprintf(stderr, "[STATE] t=%lu vt=%08X %s sub=%u next=%08X phase=%u\n",
                    (unsigned long)(now - t0), vt, state_name(vt), sub, next, phase);
            fflush(stderr);
            last_vt = vt;
            last_next = next;
            last_sub = sub;
        }
        /* Rewritten every sample so a reader sees a fresh "seconds in state". */
        {
            FILE *f;
            extern uint32_t xbox_Nv2aDrawCount(void);
            /* 7th/8th fields: the active frontend screen (vtable and id,
             * ssx3_overrides.c sub_00204400) -- which menu page is up. */
            /* 9th: that screen's live state (5 = active; it moves on when the
             * screen closes) -- the in-game HUD is left out of fields 7/8,
             * so a pause menu over the race shows as the pause menu. */
            extern volatile uint32_t g_ssx3_ui_screen_vt, g_ssx3_ui_screen_id, g_ssx3_ui_screen_ptr;
            uint32_t sp = g_ssx3_ui_screen_ptr;
            unsigned sst = sp ? (unsigned)((MEM16(sp + 0x1C) >> 8) & 0x3F) : 0;
            snprintf(line, sizeof line, "%08X %s %lu %lu %u %u %08X %d %u\n", vt, state_name(vt),
                     (unsigned long)(now - t0), (unsigned long)((now - since) / 1000), sub,
                     xbox_Nv2aDrawCount(), g_ssx3_ui_screen_vt, (int)g_ssx3_ui_screen_id, sst);
            f = fopen("game_state.tmp", "w");
            if (f) {
                fputs(line, f);
                fclose(f);
                #ifdef _WIN32
                MoveFileExA("game_state.tmp", "game_state.txt", MOVEFILE_REPLACE_EXISTING);
#else
                rename("game_state.tmp", "game_state.txt");
#endif
            }
        }
    }
}

void ssx3_GameStateStart(void)
{
    const char *e = getenv("RECOMP_GAME_STATE");
    if (e && e[0] == '0')
        return;
    CloseHandle(CreateThread(NULL, 0, game_state_thread, NULL, 0, NULL));
}

/* Rider i's position (world, Z-up), for the motion blur (vkr_backend.c keeps
 * player 1 sharp and blurs the others by their own motion). The rider manager
 * [[[0x369534]+0x74]+0xC] (vtable 0x0028216C) holds the riders at
 * +0x28 + 4*i, slot 0 = player 1 (vtable 0x00282B08 in a race), then the
 * computer riders (0x00282520); a rider's position is at +0x180. A slot
 * counts when its object has one of those two vtables. 0 outside a race. */
int ssx3_RiderPos(int i, float out[3])
{
    uint32_t g = MEM32(0x00369534u), a, m, b, b0;
    if (i < 0 || i >= SSX3_RIDERS || !guest_ok(g) || !guest_ok(a = MEM32(g + 0x74))
            || !guest_ok(m = MEM32(a + 0xC)) || MEM32(m) != 0x0028216Cu
            || !guest_ok(b0 = MEM32(m + 0x28)) || !guest_ok(b = MEM32(m + 0x28 + 4u * (uint32_t)i))
            || (MEM32(b) != MEM32(b0) && MEM32(b) != 0x00282520u))
        return 0;
    out[0] = MEMF(b + 0x180);
    out[1] = MEMF(b + 0x184);
    out[2] = MEMF(b + 0x188);
    return isfinite(out[0]) && isfinite(out[1]) && isfinite(out[2]);
}

int ssx3_PlayerPos(float out[3])
{
    return ssx3_RiderPos(0, out);
}
