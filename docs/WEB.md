# Web build (WebAssembly)

Branch `wasm`, based on `main` (no remaster shaders). The same lifted C and
toolkit, compiled with Emscripten into `build-web/ssx3_recomp.{js,wasm}` plus
a page (`web/`). Rendering through WebGPU (the Vulkan renderer's port, below;
`?gpu=sw` = the toolkit's software rasteriser, also the fallback); sound through an AudioWorklet; keyboard and Gamepad API input.

## Build

```bash
scripts/setup.sh && scripts/regen.sh     # as for the native builds
# Emscripten SDK once, no root needed:
git clone --depth 1 https://github.com/emscripten-core/emsdk ~/emsdk
~/emsdk/emsdk install latest && ~/emsdk/emsdk activate latest
source scripts/web-env.sh                 # emsdk + cmake/ninja (Fedora sysroot)
emcmake cmake -S . -B build-web -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DXBOXRECOMP_DIR=$PWD/../xboxrecomp
ninja -C build-web                        # ~1.5 min on 12 jobs; 41 MB .wasm
```

## Play

```bash
tools/serve_web.py                        # http://127.0.0.1:8080/
```

Open it in a current Chrome, Edge or Firefox. The page asks for your game
folder (default.xbe + data/, the PAL disc) once and copies it (2.2 GB) into
the browser's origin-private file system (OPFS); saves and the emulated hard
disk live there too, so they survive reloads. "Replace game files" picks
again; `?fresh` drops the copy.

- The page must be cross-origin isolated (COOP/COEP) for threads; the
  server sends the headers. `localhost` counts as a secure context.
- **Private use only.** The module is SSX 3's own code, recompiled: serve it
  to yourself (the server binds to 127.0.0.1), never publicly.
- `?env=RECOMP_X=value` (repeatable) sets any of the native build's knobs.
- Automated runs: `tools/serve_web.py --game disk/SSX3` also serves the rip
  under `/game/`; `?import=server&autostart` copies it and boots unattended.

## How it differs from the native builds

| Native | Web |
|---|---|
| Guest 4 GB laid out 1:1 at a host offset; mirrors / contiguous window / tiled aperture are extra views of the same pages | One 2 GB wasm memory, no aliasing: `XBOX_PTR` goes through a page map, 1 MB granularity (`g_xbox_pagemap`, toolkit `src/kernel/guest_wasm.c`). Host code uses `XBOX_HOST(va)` / `xbox_GuestToHost` (`guest_ptr.h`) instead of `offset + va`. |
| Device registers (APU, OHCI, AC'97) are unmapped pages; faults are decoded (VEH / SIGSEGV) | No faults: every 8/16/32-bit guest access checks for the MCPX span (0xFE800000-0xFEFFFFFF) and calls `xbox_mmio_read/write` (main.c). Stores are explicit `STn(addr, v)` (`tools/explicit_stores.py`, run by `regen.sh`), so `MEMn` is an rvalue on the web and a missed store is a compile error. |
| Guest threads = host threads | pthreads on Web Workers (`PROXY_TO_PTHREAD`: `main()` runs on a worker, the page thread stays free); 40 pre-spawned. |
| Files from the game folder | WASMFS + OPFS backend mounted at `/opfs` (`RECOMP_GAME_DIR=/opfs/ssx3/game`, saves `/opfs/ssx3/saves`, hard disk `/opfs/ssx3/system`). |
| Window (D3D11 / SDL) | `fb_present_web.c`: RGBA frame per flip, the page copies it into a canvas each animation frame (`web_frame`). |
| XAudio2 / SDL audio | `apu_xaudio2.c` web backend: ring in wasm memory, read by `web/audio-worklet.js`. |
| XInput / SDL pads, window keys | Gamepad API polled by the page (`web_pad`), keys by `KeyboardEvent.code` (`web_key`); same key map as `play.bat`. |
| Thread dumps, watchpoints, crash handler | Not available (no signals, no single-step). |

## WebGPU renderer (2026-10-17, the default)

Race on Snow Jam ~49 FPS in headless Chromium on lavapipe (CPU Vulkan, this
aarch64 box); the software rasteriser does 4-8.

- `src/web/wgr.c` implements `src/vk/vkr.h` (so `vkr_backend.c` is reused):
  it records each frame -- commands plus all data -- into one of two slots in
  shared memory; slot states FREE -> RECORDING -> SUBMITTED -> GPU -> DONE ->
  FREE are futex words shared with JS.
- `web/gpu.js` (page thread) replays a slot through WebGPU, presents to the
  canvas through the gamma LUT, reads the guest-size frame back into the
  slot's buffer and marks it DONE. `web/xbox.wgsl` is the GLSL shaders ported
  (no vertex buffers: vertices/indices pulled from storage; points and lines
  expanded to quads; y flipped; border addressing in the shader).
- Page: WebGPU unless `?gpu=sw` or no adapter; sets `RECOMP_WEBGPU=1` for the C side.
- Stencil (the snow trails use it, see the Vulkan renderer): depth32float-stencil8
  where the adapter has it, else depth24plus-stencil8; stencil state is part
  of the pipeline key, the reference is set per draw.

The "hang after two frames" was the GPU executor trapping (memory access out
of bounds) in `nv2a_exec_tex_info`: its texture hash read guest memory as
`xbox_GetMemoryOffset() + offset`, the native 1:1 layout, and SSX 3's
textures are in the contiguous window. Now `XBOX_HOST`. Only the GPU backend
calls it, so the software path never hit it. Any other `xbox_GetMemoryOffset()
+ va` left in the toolkit is the same bug waiting (`nv2a_pb_exec.c` still has
a dozen `mem + ...` uses; the software path's work, so far fine).

Testing: `node tools/web_test.cjs --secs N --every S [--query "&env=..."]`
(Playwright, full Chromium new-headless: it has a WebGPU adapter on the real
GPU, the headless shell has none; `--pause-at S` dumps the page thread's JS
stack; `--workers` adds workers' console; `--worker-errors` logs uncaught
worker exceptions with their wasm stacks -- start there when frames stop;
`--video` records the run to `work/web-run/video/`). `?wgstats` logs
[WGSTATS] once a second: JS replay ms, GPU latency (submit -> readback), MB
uploaded and draws per frame. Headless Chromium on this box only offers
llvmpipe (no Honeykrisp adapter, whatever the flags): race at 1440x1080
~45 FPS, GPU latency 20-35 ms; at `?env=RECOMP_RES=1` a steady 60 (GPU 11 ms,
JS 1 ms) -- fill-bound on the CPU rasteriser, not the renderer.
Setup: `(cd work/pw && npm install playwright@1.48.2 && npx playwright install chromium)`;
on aarch64 Linux (no Playwright Chromium) `npm install playwright` and run with
`PW_CHROMIUM=/usr/bin/chromium-browser`.
`?hb` logs a page heartbeat each second; `?wgskip=draw|vsh|present|readback|attach`.
