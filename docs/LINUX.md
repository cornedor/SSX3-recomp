# Linux build

```bash
scripts/setup.sh && scripts/regen.sh      # as on Windows (XBE in disk/SSX3 or ../SSX3)
ln -s disk/SSX3 game                      # exe opens game/default.xbe from the cwd
cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build-linux && build-linux/ssx3_recomp
```

Deps: `tools/linux/install_deps.sh` in the toolkit (SDL2, libepoxy, OpenSSL),
plus CMake, Ninja, Vulkan headers/loader and glslc. Without root (Fedora):
`scripts/linux-sysroot.sh` unpacks them into `~/.local/ssx3-sysroot`; then
`source ~/.local/ssx3-sysroot/env.sh` before cmake/ninja.
Tested on aarch64 (Asahi) and x86_64 (Fedora 44, GTX 1080) with GCC 16.

Play: `tools/play.sh` (keyboard = player 1, F11 / Alt+Enter fullscreen).

## Wayland (2026-10-16)

On a Wayland session the window is native Wayland (SDL driver
`wayland,x11`; `RECOMP_WAYLAND=0` = let SDL pick, i.e. X11 via Xwayland;
`SDL_VIDEODRIVER` still wins). The window opens 4:3 at 3/4 of the display
height (`RECOMP_WINDOW=WxH`, `RECOMP_FULLSCREEN=1`), HiDPI-aware, the frame
scaled into it with linear filtering, app id `ssx3-recomp`. With
`RECOMP_WIDESCREEN=auto` (default) the frame follows the window's shape, so
fullscreen on a 16:9 screen gives 16:9. The log's `[FBWIN]` line names the
driver.

Headless test runs (no desktop session, e.g. over SSH): start a private
compositor and point the game at it:

```bash
XDG_RUNTIME_DIR=/run/user/$(id -u) kwin_wayland --virtual --width 1920 --height 1080 --socket wayland-ssx3 &
XDG_RUNTIME_DIR=/run/user/$(id -u) WAYLAND_DISPLAY=wayland-ssx3 build-linux/ssx3_recomp
```

A fresh `RECOMP_SYSTEM_DIR` makes the first boot format the cache partition
and quit (`HalReturnToFirmware`, launch data type 1); the second boot runs.

## Status (2026-10-14)

Boots through intro to the menus on aarch64 (Asahi, M2) with a window (SDL2,
`RECOMP_FB_WINDOW`, default in main.c). Not yet checked: playing, audio, input,
speed. Run with `timeout -s KILL` (plain `timeout` leaves the process behind).

What makes it work on POSIX:

- `src/mmio_posix.c`: SIGSEGV handler decodes the host load/store (aarch64
  or x86-64) for APU / OHCI registers (device callbacks,
  `xbox_OhciMmioAccess`). Unknown forms are declined and reported.
- AC'97 bus-master reset (RR bit) trap: page made read-only, writes emulated in
  `main.c` (`ac97_arm`).
- Hardware TSO (`PR_SET_MEM_MODEL`, Apple cores) for the guest's x86 ordering.
- Guest memory reserved at 0x100000000000 so the 0xFD000000+ apertures fit.
- Case-insensitive path resolution (`fix_path_case`), Partition0 image.
- `video/fb_present_sdl.c`: framebuffer window, BMP dump, `RECOMP_SNAP_TRIGGER`.
- Thread dump (`RECOMP_THREAD_DUMP_SECS`, SIGUSR2) and `RECOMP_PEEK_EVERY_MS`.

## Not ported

- `RECOMP_WATCH` (hardware watchpoints), Toolhelp-based dumps.
- Audio: SDL2 audio queue behind the `xa2_*` interface (apu_xaudio2.c), not checked by ear. Gamepad/keyboard: untested.
- SEH (`RtlUnwind`, `RaiseException`) is a shim in `win32_compat.c`.
- x86-64 hosts: `mmio_posix_step` decodes the forms compilers emit for one
  access (MOV both ways and immediates, MOVZX/MOVSX, ALU r/m,r and r,r/m,
  group-1 immediates, TEST, NOT/NEG, INC/DEC); SSE, string ops and LOCK are
  declined and reported as a crash. Boot to race verified on x86_64.
