// WebGPU executor for src/web/wgr.c, on the page thread.
//
// wgr.c records each frame -- commands plus every byte they read -- into one
// of two slots in the wasm memory and hands it over at the flip. This replays
// a slot through WebGPU: one upload of its data, one command encoder, the
// frame shown on the canvas, and the guest-sized image read back into the
// slot's readback buffer for the backend's flip thread (src/vk/vkr_backend.c),
// which copies it into guest memory.
'use strict';

const WGR = {
  TARGET: 1, SCISSOR: 2, CLEAR: 3, TEX_CREATE: 4, TEX_UPDATE: 5, TEX_FREE: 6, DRAW: 7, FLIP: 8,
};
const S_FREE = 0, S_RECORDING = 1, S_SUBMITTED = 2, S_DONE = 3, S_GPU = 4;
const DATA_BYTES = 32 << 20;          // WGR_DATA_BYTES
const DRAW_BYTES = 512;               // WGR_DRAW_BYTES (bound size of a Draw block)

const GL_BLEND = {
  0x0000: 'zero', 0x0001: 'one', 0x0300: 'src', 0x0301: 'one-minus-src',
  0x0302: 'src-alpha', 0x0303: 'one-minus-src-alpha', 0x0304: 'dst-alpha',
  0x0305: 'one-minus-dst-alpha', 0x0306: 'dst', 0x0307: 'one-minus-dst',
  0x0308: 'src-alpha-saturated', 0x8001: 'constant', 0x8002: 'one-minus-constant',
  0x8003: 'constant', 0x8004: 'one-minus-constant',      // constant alpha: see setBlendConstant
};
const GL_EQ = { 0x8006: 'add', 0x800A: 'subtract', 0x800B: 'reverse-subtract', 0x8007: 'min', 0x8008: 'max' };
const DEPTH_FN = ['never', 'less', 'equal', 'less-equal', 'greater', 'not-equal', 'greater-equal', 'always'];
const STENCIL_OP = ['keep', 'zero', 'replace', 'increment-clamp', 'decrement-clamp', 'invert',
  'increment-wrap', 'decrement-wrap'];          // wgr.c stencil_op()

const PRESENT_WGSL = `
@group(0) @binding(0) var img: texture_2d<f32>;
@group(0) @binding(1) var smp: sampler;
@group(0) @binding(2) var lut: texture_2d<f32>;
struct V { @builtin(position) pos: vec4f, @location(0) uv: vec2f };
@vertex fn vs(@builtin(vertex_index) i: u32) -> V {
  var p = array<vec2f, 3>(vec2f(-1.0, -1.0), vec2f(3.0, -1.0), vec2f(-1.0, 3.0));
  var o: V;
  o.pos = vec4f(p[i], 0.0, 1.0);
  o.uv = vec2f(p[i].x * 0.5 + 0.5, 0.5 - p[i].y * 0.5);
  return o;
}
fn lk(x: f32, ch: i32) -> f32 {
  let t = textureLoad(lut, vec2i(i32(clamp(x, 0.0, 1.0) * 255.0 + 0.5), 0), 0);
  return t[ch];
}
@fragment fn fs_present(v: V) -> @location(0) vec4f {
  let c = textureSampleLevel(img, smp, v.uv, 0.0);
  return vec4f(lk(c.r, 0), lk(c.g, 1), lk(c.b, 2), 1.0);
}
@fragment fn fs_down(v: V) -> @location(0) vec4f {
  return textureSampleLevel(img, smp, v.uv, 0.0);
}`;

class WebGPURenderer {
  static async create(canvas, log) {
    if (!navigator.gpu) return null;
    const adapter = await navigator.gpu.requestAdapter({ powerPreference: 'high-performance' });
    if (!adapter) return null;
    // The NV2A's Z24S8 has a stencil buffer SSX 3 uses (the snow trails fix up
    // depth through it): depth32float-stencil8 where available.
    const d32s8 = adapter.features.has('depth32float-stencil8');
    const device = await adapter.requestDevice({
      requiredFeatures: d32s8 ? ['depth32float-stencil8'] : [],
      requiredLimits: { maxStorageBufferBindingSize: Math.min(DATA_BYTES, adapter.limits.maxStorageBufferBindingSize) },
    });
    const r = new WebGPURenderer(device, canvas, log);
    r.depthFmt = d32s8 ? 'depth32float-stencil8' : 'depth24plus-stencil8';
    const info = adapter.info || {};
    log(`[WEBGPU] ${info.vendor || '?'} ${info.architecture || ''} ${info.description || ''}`.trim());
    try {
      await r.init();
    } catch (e) {
      device.destroy();
      throw e;
    }
    // Only now take the canvas: a canvas that has a WebGPU context can never
    // get the 2D one the software fallback draws with.
    r.ctx = canvas.getContext('webgpu');
    r.ctx.configure({ device, format: r.format, alphaMode: 'opaque' });
    return r;
  }

  constructor(device, canvas, log) {
    this.dev = device;
    this.canvas = canvas;
    this.log = log;
    this.ctx = null;
    this.format = navigator.gpu.getPreferredCanvasFormat();
    this.pipes = new Map();
    this.samplers = new Map();
    this.texs = new Map();
    this.bg1 = new Map();
    this.slots = [];
    this.target = null;
    this.frames = 0;
    this.skip = new URLSearchParams(location.search).get('wgskip');   // debugging: draw | vsh
    this.errors = 0;
    // ?wgstats: once a second, [WGSTATS] frames, JS replay ms (avg/max), GPU
    // latency submit -> readback mapped (avg/max), MB uploaded, draws.
    this.stats = new URLSearchParams(location.search).has('wgstats')
      ? { t: performance.now(), n: 0, js: 0, jsMax: 0, gpu: 0, gpuMax: 0, bytes: 0, draws: 0, idle: 0 } : null;
    device.addEventListener('uncapturederror', (e) => {
      if (this.errors++ < 20) log('[WEBGPU] error: ' + e.error.message);
    });
    device.lost.then((i) => { log('[WEBGPU] device lost: ' + i.message); this.fail(); });
  }

  async init() {
    const d = this.dev;
    d.pushErrorScope('validation');
    const code = await (await fetch('xbox.wgsl')).text();
    this.mod = d.createShaderModule({ code });
    const ci = await this.mod.getCompilationInfo();
    for (const m of ci.messages) this.log(`[WEBGPU] xbox.wgsl:${m.lineNum}:${m.linePos} ${m.type}: ${m.message}`);
    this.bgl0 = d.createBindGroupLayout({ entries: [
      { binding: 0, visibility: GPUShaderStage.VERTEX | GPUShaderStage.FRAGMENT,
        buffer: { type: 'uniform', hasDynamicOffset: true, minBindingSize: DRAW_BYTES } },
      { binding: 1, visibility: GPUShaderStage.VERTEX, buffer: { type: 'read-only-storage' } },
    ] });
    const e1 = [];
    for (let k = 0; k < 4; k++) e1.push({ binding: k, visibility: GPUShaderStage.FRAGMENT, texture: {} });
    for (let k = 0; k < 4; k++) e1.push({ binding: 4 + k, visibility: GPUShaderStage.FRAGMENT, sampler: {} });
    this.bgl1 = d.createBindGroupLayout({ entries: e1 });
    this.layout = d.createPipelineLayout({ bindGroupLayouts: [this.bgl0, this.bgl1] });
    const white = d.createTexture({ size: [1, 1], format: 'bgra8unorm',
      usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST });
    d.queue.writeTexture({ texture: white }, new Uint8Array([255, 255, 255, 255]), { bytesPerRow: 4 }, [1, 1]);
    this.texs.set(0, { tex: white, view: white.createView() });

    this.pmod = d.createShaderModule({ code: PRESENT_WGSL });
    this.lut = d.createTexture({ size: [256, 1], format: 'bgra8unorm',
      usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST });
    const ident = new Uint8Array(1024);
    for (let i = 0; i < 256; i++) ident.set([i, i, i, 255], i * 4);
    d.queue.writeTexture({ texture: this.lut }, ident, { bytesPerRow: 1024 }, [256, 1]);
    this.lutSerial = -1;
    this.linear = d.createSampler({ magFilter: 'linear', minFilter: 'linear' });
    this.presentPipe = d.createRenderPipeline({ layout: 'auto',
      vertex: { module: this.pmod, entryPoint: 'vs' },
      fragment: { module: this.pmod, entryPoint: 'fs_present', targets: [{ format: this.format }] } });
    this.downPipe = d.createRenderPipeline({ layout: 'auto',
      vertex: { module: this.pmod, entryPoint: 'vs' },
      fragment: { module: this.pmod, entryPoint: 'fs_down', targets: [{ format: 'bgra8unorm' }] } });
    const err = await d.popErrorScope();
    if (err) throw new Error(err.message);
  }

  // From wgr.c's vkr_create (proxied to this thread): the control block.
  attach(mod, ctl) {
    if (this.skip === 'attach') return false;
    this.mod_ = mod;
    this.u8 = mod.HEAPU8;
    this.u32 = new Uint32Array(this.u8.buffer);
    this.i32 = new Int32Array(this.u8.buffer);
    this.f32 = new Float32Array(this.u8.buffer);
    this.ctl = ctl >>> 2;
    if (this.u32[this.ctl] !== 0x31524757) { this.log('[WEBGPU] bad control block'); return false; }
    this.nslots = this.u32[this.ctl + 1];
    this.next = 0;
    this.run();
    return true;
  }

  slotWord(s, k) { return this.ctl + 4 + s * 8 + k; }   // WgrSlot: 8 words

  fail() {
    if (!this.ctl) return;
    this.u32[this.ctl + 3] = 1;
    for (let s = 0; s < this.nslots; s++) Atomics.notify(this.i32, this.slotWord(s, 0));
  }

  async run() {
    const sub = this.ctl + 2;
    for (;;) {
      const seen = Atomics.load(this.i32, sub);
      const st = Atomics.load(this.i32, this.slotWord(this.next, 0));
      if (st !== S_SUBMITTED) {
        if (Atomics.waitAsync) {
          const w = Atomics.waitAsync(this.i32, sub, seen, 1000);
          if (w.async) await w.value;
        } else {
          await new Promise((r) => setTimeout(r, 1));
        }
        continue;
      }
      // Taken: rendering, then reading back (S_DONE once the pixels are in).
      Atomics.store(this.i32, this.slotWord(this.next, 0), S_GPU);
      if (this.frames < 4) console.log(`[WEBGPU] take slot ${this.next} #${this.frames}`);
      const t0 = performance.now();
      try {
        this.frame(this.next);
      } catch (e) {
        this.log('[WEBGPU] frame failed: ' + (e.stack || e));
        this.done(this.next);
      }
      if (this.stats) {
        const st = this.stats, dt = performance.now() - t0;
        st.n++; st.js += dt; st.jsMax = Math.max(st.jsMax, dt);
        st.bytes += this.u32[this.slotWord(this.next, 4)];
        if (performance.now() - st.t >= 1000) {
          const n = st.n, f = (v) => v.toFixed(1);
          this.log(`[WGSTATS] ${n} frames, js ${f(st.js / n)}/${f(st.jsMax)} ms, gpu ${f(st.gpu / n)}/${f(st.gpuMax)} ms, `
            + `${(st.bytes / n / 1048576).toFixed(2)} MB/frame, ${(st.draws / n).toFixed(0)} draws/frame`);
          Object.assign(st, { t: performance.now(), n: 0, js: 0, jsMax: 0, gpu: 0, gpuMax: 0, bytes: 0, draws: 0 });
        }
      }
      this.next = (this.next + 1) % this.nslots;
    }
  }

  done(s) {
    Atomics.store(this.i32, this.slotWord(s, 0), S_DONE);
    Atomics.notify(this.i32, this.slotWord(s, 0));
  }

  slotRes(s) {
    let r = this.slots[s];
    if (!r) {
      const data = this.dev.createBuffer({ size: DATA_BYTES,
        usage: GPUBufferUsage.STORAGE | GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST | GPUBufferUsage.COPY_SRC });
      const bg0 = this.dev.createBindGroup({ layout: this.bgl0, entries: [
        { binding: 0, resource: { buffer: data, size: DRAW_BYTES } },
        { binding: 1, resource: { buffer: data } },
      ] });
      r = this.slots[s] = { data, bg0, rb: null, rbSize: 0 };
    }
    return r;
  }

  sampler(key) {
    let s = this.samplers.get(key);
    if (!s) {
      const mode = (a) => (a === 2 ? 'mirror-repeat' : a === 3 || a === 4 || a === 5 ? 'clamp-to-edge' : 'repeat');
      const lin = (key >> 6) & 1 ? 'linear' : 'nearest';
      s = this.dev.createSampler({ addressModeU: mode(key & 7), addressModeV: mode((key >> 3) & 7),
        magFilter: lin, minFilter: lin });
      this.samplers.set(key, s);
    }
    return s;
  }

  bindTextures(ids, samps) {
    const key = ids.join(',') + '|' + samps.join(',');
    let bg = this.bg1.get(key);
    if (!bg) {
      const e = [];
      for (let k = 0; k < 4; k++) {
        const t = this.texs.get(ids[k]) || this.texs.get(0);
        e.push({ binding: k, resource: t.view });
      }
      for (let k = 0; k < 4; k++) e.push({ binding: 4 + k, resource: this.sampler(samps[k]) });
      bg = this.dev.createBindGroup({ layout: this.bgl1, entries: e });
      this.bg1.set(key, bg);
    }
    return bg;
  }

  pipeline(k0, k1, sk, sm) {
    const key = `${k0},${k1},${sk},${sm}`;
    let p = this.pipes.get(key);
    if (p) return p;
    const blendOn = k0 & 1, mask = (k0 >> 1) & 15, dtest = (k0 >> 5) & 1, dwrite = (k0 >> 6) & 1;
    const dfunc = (k0 >> 7) & 7, wdepth = (k0 >> 10) & 1, eqGl = k0 >>> 16;
    const target = { format: 'bgra8unorm', writeMask: mask };
    if (blendOn) {
      const op = GL_EQ[eqGl] || 'add';
      let src = GL_BLEND[k1 & 0xFFFF] || 'one', dst = GL_BLEND[k1 >>> 16] || 'one';
      if (op === 'min' || op === 'max') { src = 'one'; dst = 'one'; }
      const c = { srcFactor: src, dstFactor: dst, operation: op };
      target.blend = { color: c, alpha: c };
    }
    const t0 = performance.now();
    p = this.dev.createRenderPipeline({
      layout: this.layout,
      vertex: { module: this.mod, entryPoint: 'vs_main' },
      fragment: { module: this.mod, entryPoint: wdepth ? 'fs_main_wdepth' : 'fs_main', targets: [target] },
      primitive: { topology: 'triangle-list', cullMode: 'none' },
      depthStencil: { format: this.depthFmt,
        depthWriteEnabled: !!(dtest && dwrite), depthCompare: dtest ? DEPTH_FN[dfunc] : 'always',
        ...this.stencilState(sk, sm) },
    });
    if (this.pipes.size < 400 && this.pipes.size % 10 === 0)
      this.log(`[WEBGPU] pipeline ${this.pipes.size}: ${(performance.now() - t0).toFixed(1)} ms`);
    this.pipes.set(key, p);
    return p;
  }

  // wgr.c's stencil word: bit 0 test, 1-3 func, 4-6 fail, 7-9 zfail, 10-12
  // zpass; masks read | write << 8. Test off: never touched.
  stencilState(sk, sm) {
    if (!(sk & 1)) return { stencilReadMask: 0, stencilWriteMask: 0 };
    const f = { compare: DEPTH_FN[(sk >> 1) & 7], failOp: STENCIL_OP[(sk >> 4) & 7],
      depthFailOp: STENCIL_OP[(sk >> 7) & 7], passOp: STENCIL_OP[(sk >> 10) & 7] };
    return { stencilFront: f, stencilBack: f, stencilReadMask: sm & 0xFF, stencilWriteMask: (sm >> 8) & 0xFF };
  }

  clearPipe(flags) {
    const key = 'clear' + flags;
    let p = this.pipes.get(key);
    if (!p) {
      p = this.dev.createRenderPipeline({
        layout: this.layout,
        vertex: { module: this.mod, entryPoint: 'vs_clear' },
        fragment: { module: this.mod, entryPoint: 'fs_clear',
          targets: [{ format: 'bgra8unorm', writeMask: flags & 1 ? GPUColorWrite.ALL : 0 }] },
        primitive: { topology: 'triangle-list' },
        depthStencil: { format: this.depthFmt, depthWriteEnabled: !!(flags & 2), depthCompare: 'always',
          ...(flags & 4 ? { stencilFront: { passOp: 'replace' }, stencilBack: { passOp: 'replace' },
                            stencilWriteMask: 0xFF } : { stencilWriteMask: 0 }) },
      });
      this.pipes.set(key, p);
    }
    return p;
  }

  newTarget(w, h, sw, sh) {
    const d = this.dev, old = this.target;
    const t = { w, h, sw, sh, valid: false };
    t.color = d.createTexture({ size: [sw, sh], format: 'bgra8unorm',
      usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.COPY_SRC | GPUTextureUsage.TEXTURE_BINDING });
    t.depth = d.createTexture({ size: [sw, sh], format: this.depthFmt, usage: GPUTextureUsage.RENDER_ATTACHMENT });
    t.colorView = t.color.createView();
    t.depthView = t.depth.createView();
    if (sw !== w || sh !== h) {
      t.down = d.createTexture({ size: [w, h], format: 'bgra8unorm',
        usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.COPY_SRC });
      t.downView = t.down.createView();
      t.downBg = d.createBindGroup({ layout: this.downPipe.getBindGroupLayout(0), entries: [
        { binding: 0, resource: t.colorView }, { binding: 1, resource: this.linear }] });
    }
    t.presentBg = d.createBindGroup({ layout: this.presentPipe.getBindGroupLayout(0), entries: [
      { binding: 0, resource: t.colorView }, { binding: 1, resource: this.linear },
      { binding: 2, resource: this.lut.createView() }] });
    if (this.canvas.width !== sw || this.canvas.height !== sh) {
      this.canvas.width = sw;
      this.canvas.height = sh;
    }
    this.target = t;
    if (old) { old.color.destroy(); old.depth.destroy(); if (old.down) old.down.destroy(); }
    this.log(`[WEBGPU] target ${w}x${h}, rendered at ${sw}x${sh}`);
  }

  frame(s) {
    const u32 = this.u32, f32 = this.f32, d = this.dev;
    const cmdPtr = u32[this.slotWord(s, 1)] >>> 2, nwords = u32[this.slotWord(s, 2)];
    const dataPtr = u32[this.slotWord(s, 3)], dataBytes = u32[this.slotWord(s, 4)];
    const res = this.slotRes(s);
    if (dataBytes)
      d.queue.writeBuffer(res.data, 0, this.u8.subarray(dataPtr, dataPtr + ((dataBytes + 3) & ~3)));
    const enc = d.createCommandEncoder();
    let pass = null, sc = [0, 0, 0, 0], flipped = false;
    const t = () => this.target;
    const endPass = () => { if (pass) { pass.end(); pass = null; } };
    const beginPass = () => {
      if (pass) return pass;
      const tg = t();
      pass = enc.beginRenderPass({
        colorAttachments: [{ view: tg.colorView, loadOp: tg.valid ? 'load' : 'clear',
          clearValue: [0, 0, 0, 0], storeOp: 'store' }],
        depthStencilAttachment: { view: tg.depthView, depthLoadOp: tg.valid ? 'load' : 'clear',
          depthClearValue: 1.0, depthStoreOp: 'store',
          stencilLoadOp: tg.valid ? 'load' : 'clear', stencilClearValue: 0, stencilStoreOp: 'store' },
      });
      tg.valid = true;
      pass.setViewport(0, 0, tg.sw, tg.sh, 0, 1);
      applySc();
      return pass;
    };
    const applySc = () => {
      const tg = t();
      const x = Math.min(sc[0], tg.sw), y = Math.min(sc[1], tg.sh);
      pass.setScissorRect(x, y, Math.min(sc[2], tg.sw - x), Math.min(sc[3], tg.sh - y));
    };

    for (let i = cmdPtr, end = cmdPtr + nwords; i < end;) {
      const op = u32[i] & 0xFF, n = u32[i] >>> 8, a = i + 1;
      i = a + n;
      switch (op) {
      case WGR.TARGET:
        endPass();
        this.newTarget(u32[a], u32[a + 1], u32[a + 2], u32[a + 3]);
        break;
      case WGR.SCISSOR:
        sc = [u32[a], u32[a + 1], u32[a + 2], u32[a + 3]];
        if (pass) applySc();
        break;
      case WGR.CLEAR: {
        if (!t()) break;
        const p = beginPass();
        p.setPipeline(this.clearPipe(u32[a + 1]));
        p.setStencilReference(u32[a + 2]);
        p.setBindGroup(0, res.bg0, [u32[a]]);
        p.setBindGroup(1, this.bindTextures([0, 0, 0, 0], [0, 0, 0, 0]));
        p.draw(3);
        break;
      }
      case WGR.TEX_CREATE: {
        endPass();
        const id = u32[a], w = u32[a + 1], h = u32[a + 2];
        const tex = d.createTexture({ size: [w, h], format: 'bgra8unorm',
          usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST });
        enc.copyBufferToTexture({ buffer: res.data, offset: u32[a + 3], bytesPerRow: u32[a + 4], rowsPerImage: h },
                                { texture: tex }, [w, h]);
        this.texs.set(id, { tex, view: tex.createView(), w, h });
        break;
      }
      case WGR.TEX_UPDATE: {
        const tx = this.texs.get(u32[a]);
        if (!tx) break;
        endPass();
        enc.copyBufferToTexture({ buffer: res.data, offset: u32[a + 1], bytesPerRow: u32[a + 2], rowsPerImage: tx.h },
                                { texture: tx.tex }, [tx.w, tx.h]);
        break;
      }
      case WGR.TEX_FREE: {
        const tx = this.texs.get(u32[a]);
        if (tx) { tx.tex.destroy(); this.texs.delete(u32[a]); this.bg1.clear(); }
        break;
      }
      case WGR.DRAW: {
        if (!t() || this.skip === 'draw' || (this.skip === 'vsh' && this.f32[(dataPtr + u32[a]) / 4 + 94] !== 0)) break;
        const p = beginPass();
        const k0 = u32[a + 2], k1 = u32[a + 3];
        p.setPipeline(this.pipeline(k0, k1, u32[a + 16], u32[a + 17]));
        p.setStencilReference(u32[a + 18]);
        p.setBindGroup(0, res.bg0, [u32[a]]);
        p.setBindGroup(1, this.bindTextures(
          [u32[a + 4], u32[a + 5], u32[a + 6], u32[a + 7]], [u32[a + 8], u32[a + 9], u32[a + 10], u32[a + 11]]));
        const bc = a + 12;
        if ((k0 & 1) && ((k1 & 0xFFFF) === 0x8003 || (k1 & 0xFFFF) === 0x8004
                      || (k1 >>> 16) === 0x8003 || (k1 >>> 16) === 0x8004))
          p.setBlendConstant([f32[bc + 3], f32[bc + 3], f32[bc + 3], f32[bc + 3]]);
        else
          p.setBlendConstant([f32[bc], f32[bc + 1], f32[bc + 2], f32[bc + 3]]);
        p.draw(u32[a + 1]);
        if (this.stats) this.stats.draws++;
        break;
      }
      case WGR.FLIP:
        endPass();
        this.flip(enc, s, res, u32[a], u32[a + 1], u32[a + 2], u32[a + 4], u32[a + 5]);
        flipped = true;
        break;
      default:
        this.log(`[WEBGPU] bad command ${op} at word ${i - cmdPtr}`);
        i = end;
      }
    }
    if (!flipped) {             // never happens: a slot is only handed over at a flip
      endPass();
      d.queue.submit([enc.finish()]);
      this.done(s);
    }
  }

  flip(enc, s, res, rbPtr, w, h, gammaPtr, gammaSerial) {
    const d = this.dev, tg = this.target;
    const dbg = this.frames < 4 ? (m) => console.log('[WEBGPU] flip: ' + m) : () => {};
    dbg('start');
    if (!tg) { d.queue.submit([enc.finish()]); this.done(s); return; }
    if (gammaSerial !== this.lutSerial) {
      this.lutSerial = gammaSerial;
      d.queue.writeTexture({ texture: this.lut }, this.u8.subarray(gammaPtr, gammaPtr + 1024), { bytesPerRow: 1024 }, [256, 1]);
    }
    // The guest-sized image for guest memory.
    const bpr = (w * 4 + 255) & ~255, size = bpr * h;
    if (res.rbSize < size) {
      if (res.rb) res.rb.destroy();
      res.rb = d.createBuffer({ size, usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST });
      res.rbSize = size;
    }
    let src = tg.color;
    if (tg.down) {
      const p = enc.beginRenderPass({ colorAttachments: [{ view: tg.downView, loadOp: 'clear',
        clearValue: [0, 0, 0, 1], storeOp: 'store' }] });
      p.setPipeline(this.downPipe);
      p.setBindGroup(0, tg.downBg);
      p.draw(3);
      p.end();
      src = tg.down;
    }
    enc.copyTextureToBuffer({ texture: src }, { buffer: res.rb, bytesPerRow: bpr, rowsPerImage: h }, [w, h]);
    // And the full image on the canvas, through the title's gamma ramp.
    if (this.skip !== 'present') {
      dbg('canvas');
      const view = this.ctx.getCurrentTexture().createView();
      dbg('got canvas texture');
      const p = enc.beginRenderPass({ colorAttachments: [{ view, loadOp: 'clear', clearValue: [0, 0, 0, 1], storeOp: 'store' }] });
      p.setPipeline(this.presentPipe);
      p.setBindGroup(0, tg.presentBg);
      p.draw(3);
      p.end();
    }
    d.queue.submit([enc.finish()]);
    dbg('submitted');
    this.frames++;
    const rb = res.rb, tSub = performance.now();
    if (this.skip === 'readback') { this.done(s); return; }
    rb.mapAsync(GPUMapMode.READ, 0, size).then(() => {
      if (this.stats) {
        const g = performance.now() - tSub;
        this.stats.gpu += g; this.stats.gpuMax = Math.max(this.stats.gpuMax, g);
      }
      if (this.frames < 4) console.log('[WEBGPU] mapped');
      const src8 = new Uint8Array(rb.getMappedRange(0, size));
      if (bpr === w * 4) this.u8.set(src8, rbPtr);
      else for (let y = 0; y < h; y++) this.u8.set(src8.subarray(y * bpr, y * bpr + w * 4), rbPtr + y * w * 4);
      rb.unmap();
      this.done(s);
    }, (e) => { this.log('[WEBGPU] readback failed: ' + e); this.done(s); });
  }
}
