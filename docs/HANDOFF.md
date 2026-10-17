# Handoff — visual modernisation (2026-10-16)

Branch `crt-shader`, all of this **uncommitted** (moved by rsync from the
aarch64 Asahi machine). Linux/Vulkan build: `ninja -C build-linux`.

## Standing instruction

Keep iterating on the shaders to **modernise the look while keeping the art
direction**: the game's own lighting, colours and per-event moods (Snow Jam's
pink-lit start, Launch Time's sunset snow) stay; we add on top (detail,
glints, filtering, resolution), never repaint. Judge every change from
screenshots (A/B, same moment) before handing it to the user — the first
first shader went out unchecked and showed nothing. The user knows how snow
looks and gives fast, concrete feedback; ask them to look.

## How to see changes yourself

Never start the game unless the user asks (memory: no-app-runs); they asked
for this work, so test runs are fine. Run a second instance in its own dir
(scratchpad) so the user's game is untouched:

```bash
D=<scratch>/run; mkdir -p $D/ab; ln -s ~/Source/ssx3/game $D/game
cp -r ~/.local/share/xboxrecomp $D/system; cp -r game/UDATA game/TDATA $D/saves/
cd $D && RECOMP_AB=$D/ab,b5000,2,2000 RECOMP_AUTONAV=race \
  RECOMP_MUTE=1 RECOMP_NO_HOST_PAD=1 RECOMP_SYSTEM_DIR=$D/system RECOMP_SAVE_DIR=$D/saves \
  timeout -s KILL 40 ~/Source/ssx3/build-linux/ssx3_recomp 2> stderr.txt
```

`RECOMP_AB=<dir>,<start>,<pairs>,<interval>` writes full-resolution
`ab<k>_on.ppm` / `ab<k>_off.ppm` (the per-frame remaster features on / off, 6
flips apart); start is ms after the first flip or `b<ms>` after the race
briefing (the same camera in every run -- use this to compare two runs, e.g.
`RECOMP_REMASTER=0` vs default); `RECOMP_AB_KEEP=1` takes both as configured.
`magick x.ppm x.png`, crop/zoom with `-filter point`. zsh does not
word-split `$env` strings: pass env vars literally.

## Done

1. **Snow glint shader**: tried, not wanted in the branch (user, 2026-10-16:
   "don't push the new snow shader"). Removed from `remaster`; the last
   version with it is the local branch `remaster-snow` (not pushed).
2. **Texture mips + 16x anisotropic filtering** (`src/vk/vkr.c`
   `vkr_tex_create_ex`, sampler key bit 7; `RECOMP_VKR_MIPS=0`,
   `RECOMP_VKR_ANISO=n`): every texture had only mip 0 — distant trees,
   fences and slopes shimmered/smeared. A/B checked: clean now. Not for
   render targets updated in place or stacked cube maps.

3. **x86_64 Linux + Wayland** (2026-10-16, fedora): `src/mmio_posix.c`
   decodes x86-64 loads/stores (APU/OHCI MMIO); native Wayland window
   (`RECOMP_WAYLAND=0` = Xwayland), sized to the display, HiDPI, linear
   scaling, F11 / Alt+Enter fullscreen. `tools/play.sh`. No-root build deps:
   `scripts/linux-sysroot.sh`. Headless test runs: docs/LINUX.md.
4. **Combiner shader speed**: `R[16]` (dynamically indexed -> local memory
   on NVIDIA) became named registers behind uniform switches: GPU time per
   race frame 20 -> 9.5 ms on a GTX 1080 (41 -> 60 FPS).
5. **Remaster preset** `RECOMP_REMASTER` (default 1, `src/main.c`; 0 = plain
   look). Each part has its own flag; set it to override, 0 = off:
   - `RECOMP_MSAA=4` multisampled colour/depth/glint, resolved at the flip;
     `RECOMP_A2C=1` alpha tests as sample coverage (xbox.frag `A2C` variant,
     only alpha-tested draws use it).
   - GPU rider shadow maps (`RECOMP_VKR_GPU_RT`, default on, x
     `RECOMP_VKR_RT_SCALE=4`): swizzled 32-bit targets drawn on the GPU and
     sampled directly; the title clears them through a pitch-linear view of
     the same address, also routed. `RECOMP_SHADOW_SOFT=1.5` (map texels):
     `rt_soft.frag` soft copy. The receiver blends ZERO/ONE_MINUS_SRC_COLOR
     (black = no shadow), so the colour is averaged and alpha is the max.
     `RECOMP_RT_DEBUG=1` logs RT draws and how they are sampled. Removing
     the per-frame shadow-map uploads also cut GPU time (~9 -> ~5 ms).
   - Post pass `post.frag` (after the glints, before CRT/read-back):
     `RECOMP_SHARPEN=0.4` (CAS-like), `RECOMP_BLOOM=0.35` /
     `RECOMP_BLOOM_THRESH=0.8` (from the frame's mips), `RECOMP_VIBRANCE=0.15`,
     `RECOMP_CONTRAST=0.1`, `RECOMP_DEBAND=1`; `RECOMP_VIGNETTE` (not in the
     preset).
   - `RECOMP_DEBLOCK=32`: DXT block seams smoothed at texture decode
     (vkr_backend.c `deblock`), steps above the threshold kept.
   All A/B checked from screenshots (title, briefing, race, widescreen);
   race at 60 FPS, GPU 8-13 ms with the preset on the GTX 1080.

6. **Scene pass** (2026-10-16, `scene.frag`, `scene_ao.frag`,
   `scene_common.glsl`; at the flip after the MSAA resolve, before the glints
   and the post pass; in game only):
   - Camera: `vkr_backend.c scene_camera` runs the terrain program
     (`RECOMP_SCENE_VSH`, default E4F2B8D0) on the CPU at 4 points per frame:
     (x w, y w, z w, w) is linear in the world position (fit error ~1e-4), so
     that gives the 4x4 map; its inverse (eye-relative, depth shifted by
     zmax for float precision) reconstructs every pixel. SSX 3 world units
     are cm, Y up; c[17] is the light's direction (the sun is -c[17]).
     `RECOMP_SCENE_TRACE=1` logs the fit and the eye.
   - Runs mid-frame, before the first 2D draw (first vertex w == 1; for GPU
     programs vertex 0 is run on the CPU) after the terrain was drawn
     (`vkr_scene_run`), so the HUD -- 2D and the 3D trick meter -- is drawn
     on top untouched; with MSAA the world is resolved, processed and
     written back into the multisampled target. Frames without a 2D draw
     get it at the flip. (A per-pixel 3D mask was tried first: HUD quads
     that write depth left un-hazed boxes; removed.) Pixels nearer than `mindist` (120) are skipped.
   - `RECOMP_AO` (0.8): SAO-style, half resolution, 16 taps, radius 5 m,
     4x4 depth-aware blur, soft curve capped at 70% darkening. **AO tags**
     decide who darkens whom: every main-target draw writes its class into
     the frame's alpha (xbox.frag `TAGS` variants; the game's real alpha
     goes to the second blend source -- dual-source blending -- so its
     blending is unchanged; SSX 3 never blends with destination alpha).
     Classes by vertex program (`vkr_backend.c` `ao_class`): ground = the
     terrain programs (`RECOMP_SCENE_VSH`, E4F2B8D0), solid =
     `RECOMP_AO_SOLID` (F46BE8DC: rocks, trees, rails, signs, gates), the
     rest (riders, trails, spray, sky, HUD) neither. Only ground receives,
     only solid occludes. Depth alone (the first version) darkened thin
     beams and whole rock faces, put halos round riders and spray, and lines
     along the board trails. `RECOMP_AO_TAGS=prog` colours every draw by its
     program (to find the classes on another title), `=0` turns tags off
     (no dualSrcBlend: off too, and then AO receives nothing).
     The pass runs at the first screen-space draw (w == 1) that is not at
     the far plane: SSX 3 draws its sky sprites as screen-space quads at the
     far plane between the terrain and the objects, and they used to start
     it, so rocks, trees and riders missed every scene effect.
     `RECOMP_SCENE_TRACE=2` lists every draw of one frame (z, w, depth,
     blend, program), `RECOMP_SCENE_TRACE_AT=<ms>` that long after the race
     briefing.
     `RECOMP_HAZE` (0.35, from 50 m): towards the terrain draw's fog colour.
     `RECOMP_SHAFTS` (1): only when the sun is on screen (rare on Snow Jam:
     the sun is 63 degrees up). `RECOMP_RIM` (0.2): sun-side silhouettes
     nearer than `rimrange`. `RECOMP_MBLUR` (off; 1 = full frame-to-frame
     motion): reprojection with last frame's camera, 12 jittered taps, up
     to 96 px at 1080 lines, right up to the camera (the snow rushing past
     is where the speed shows); riders (tag 3, `RECOMP_AO_RIDER`, default
     E9FDBED0) stay sharp and are kept out of the taps. It used to skip
     everything nearer than 6 m to protect the rider, which left only the
     background moving. `RECOMP_DOF=<px>` (off; 3 is subtle): far blur,
     growing from `dofstart` (40 m) to full at `dofend` (250 m), 16-tap
     disk; taps nearer than `dofstart` are left out, so the rider does not
     bleed into the blur. Knobs: `RECOMP_SCENE_PARAMS` (radius taps start
     density rimrange near mindist gain dofstart dofend).
     `RECOMP_SCENE_DEBUG=1|2|3|4`: AO, normals, distance, AO tags.
   - `RECOMP_TEX_BICUBIC=1` (not in the preset): Catmull-Rom for magnified
     linear textures. Crisper, but it also brings out the DXT blocks; not a
     clear win on these textures.
   - Cost on the GTX 1080 at 1440x1080: scene pass 1-2 ms. Race 60 FPS, GPU
     10.5-13.5 ms with the whole preset. `RECOMP_VKR_GPUPROF` splits draws
     (the mid-frame scene pass included) / flip-time scene / post.
   - Before/after screenshots: docs/screenshots/remaster/ (Snow Jam,
     Happiness, two freestyle courses; Peak 2+ is locked in the test save;
     Metro-City's AI riders bump the player differently every run).
   - A/B: `RECOMP_AB` (above) turns the per-frame features (bicubic,
     scene, post) off for the "off" shot.

## Next ideas

- The game's 640x480 offscreen surfaces are only drawn at boot -- not worth
  moving to the GPU.
- Bicubic magnification for heavily stretched textures (backdrops).
- Verify the Windows build (all of the above is portable C/GLSL, but only
  built and run on Linux here).

## Caveats on this machine

- `fedora` is x86_64 and runs the Linux build (see point 3). No sudo: build
  with `source ~/.local/ssx3-sysroot/env.sh; ninja -C build-linux`.
- The toolkit `../xboxrecomp` has uncommitted local changes (the patch);
  refresh `patches/xboxrecomp-local.patch` if you change it.
