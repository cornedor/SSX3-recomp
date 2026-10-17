// SSX 3 web front end: game files into OPFS, then the wasm build with a canvas,
// keyboard and gamepads, and an AudioWorklet. See docs/WEB.md.
'use strict';

const $ = (id) => document.getElementById(id);
const overlay = $('overlay'), msg = $('msg'), prog = $('prog'), actions = $('actions');
const canvas = $('screen');
let ctx2d = null;                    // software frames (no WebGPU)
let gpu = null;                      // the WebGPU renderer (gpu.js)
const logEl = $('log'), stat = $('stat');

const SSX3_TITLE_ID = 0x4541003C;    // PAL, XDK 5558
const OPFS_DIR = ['ssx3', 'game'];

let mod = null;                      // the Emscripten module once running
let audio = null;                    // { ctx, node }

// ---------------------------------------------------------------- log
const logLines = [];
let logDirty = false;
// ?report: send the log and periodic canvas shots to tools/serve_web.py --sink
// (headless test runs). ?shots=N: a shot every N seconds (default 10).
const report = new URLSearchParams(location.search).has('report');
let reportBuf = [];
if (report) {
  setInterval(() => {
    if (!reportBuf.length) return;
    const body = reportBuf.join('\n') + '\n';
    reportBuf = [];
    fetch('report/log', { method: 'POST', body }).catch(() => {});
  }, 500);
  const every = +(new URLSearchParams(location.search).get('shots') || 10);
  let n = 0;
  setInterval(() => {
    if (!mod) return;
    const t = Math.round(performance.now() / 1000);
    canvas.toBlob((b) => b && fetch(`report/shot?name=t${String(t).padStart(4, '0')}`,
      { method: 'POST', body: b }).catch(() => {}), 'image/png');
    n++;
  }, every * 1000);
}
function log(line) {
  if (!line.startsWith("[GAME]")) console.log(line);
  if (report) reportBuf.push(line);
  logLines.push(line);
  if (logLines.length > 400) logLines.splice(0, logLines.length - 400);
  logDirty = true;
}
setInterval(() => {
  if (!logDirty) return;
  logDirty = false;
  const atEnd = logEl.scrollTop + logEl.clientHeight >= logEl.scrollHeight - 4;
  logEl.textContent = logLines.join('\n');
  if (atEnd) logEl.scrollTop = logEl.scrollHeight;
}, 250);

addEventListener('error', (e) => log(`[PAGE] error: ${e.message} (${e.filename}:${e.lineno})\n${e.error && e.error.stack || ''}`));
addEventListener('unhandledrejection', (e) => log(`[PAGE] unhandled: ${e.reason && (e.reason.stack || e.reason)}`));

function say(html, { progress = null, buttons = [] } = {}) {
  if (progress === null) log('[PAGE] ' + html.replace(/<[^>]+>/g, ''));
  overlay.hidden = false;
  msg.innerHTML = html;
  prog.hidden = progress === null;
  if (progress !== null) prog.value = progress;
  actions.replaceChildren(...buttons);
}

function button(text, onclick, primary = false) {
  const b = document.createElement('button');
  b.textContent = text;
  if (primary) b.className = 'primary';
  b.onclick = onclick;
  return b;
}

// ---------------------------------------------------------------- OPFS
async function opfsDir(path, create = false) {
  let d = await navigator.storage.getDirectory();
  for (const name of path) d = await d.getDirectoryHandle(name, { create });
  return d;
}

async function haveGame() {
  try {
    const d = await opfsDir(OPFS_DIR);
    const f = await (await d.getFileHandle('default.xbe')).getFile();
    return f.size > 0;
  } catch { return false; }
}

function xbeTitleId(buf) {
  const v = new DataView(buf);
  if (v.byteLength < 0x180 || v.getUint32(0, true) !== 0x48454258) return null;  // 'XBEH'
  const base = v.getUint32(0x104, true), cert = v.getUint32(0x118, true) - base;
  return cert + 12 <= v.byteLength ? v.getUint32(cert + 8, true) : null;
}

// The folder picked with <input webkitdirectory>: every file, with its path
// relative to the chosen folder. Copied into OPFS under ssx3/game/.
async function importFolder(files) {
  const list = [...files];
  if (!list.length) return;
  const strip = (p) => p.split('/').slice(1);          // drop the chosen folder's name
  const xbe = list.find((f) => strip(f.webkitRelativePath).join('/').toLowerCase() === 'default.xbe');
  if (!xbe) {
    say('<span class="bad">That folder has no <strong>default.xbe</strong>.</span> '
      + 'Pick the folder that holds default.xbe and the data folder.', { buttons: pickButtons() });
    return;
  }
  const id = xbeTitleId(await xbe.slice(0, 0x10000).arrayBuffer());
  if (id !== SSX3_TITLE_ID) {
    say('<span class="warn">This default.xbe is not SSX 3 PAL (title id '
      + (id === null ? '?' : '0x' + id.toString(16).toUpperCase()) + ', expected 0x4541003C).</span> '
      + 'The recompiled code only matches that disc.', { buttons: pickButtons() });
    return;
  }
  if (navigator.storage.persist) await navigator.storage.persist();
  const total = list.reduce((s, f) => s + f.size, 0);
  const est = navigator.storage.estimate ? await navigator.storage.estimate() : null;
  if (est && est.quota && est.quota - (est.usage || 0) < total) {
    say(`<span class="bad">Not enough browser storage: the game needs ${(total / 2 ** 30).toFixed(1)} GB, `
      + `${((est.quota - (est.usage || 0)) / 2 ** 30).toFixed(1)} GB is available to this page.</span>`);
    return;
  }
  // Start clean: a partial earlier copy must not survive.
  const root = await opfsDir(['ssx3'], true);
  try { await root.removeEntry('game', { recursive: true }); } catch {}
  let done = 0;
  for (const f of list) {
    const parts = strip(f.webkitRelativePath);
    let d = await root.getDirectoryHandle('game', { create: true });
    for (const name of parts.slice(0, -1)) d = await d.getDirectoryHandle(name, { create: true });
    const fh = await d.getFileHandle(parts[parts.length - 1], { create: true });
    const w = await fh.createWritable();
    await f.stream().pipeTo(w);
    done += f.size;
    say(`Copying the game files into this browser… <strong>${(done / 2 ** 20).toFixed(0)}`
      + ` / ${(total / 2 ** 20).toFixed(0)} MB</strong><br>${parts.join('/')}`, { progress: done / total });
  }
  log(`[WEB] copied ${list.length} files, ${(total / 2 ** 20).toFixed(0)} MB into OPFS`);
  if (params.has('autostart')) start(); else ready();
}

function pickButtons() {
  const b = [button('Choose the game folder…', () => $('pick').click(), true)];
  if (serverGame) b.push(button('Copy from this server', importFromServer));
  return b;
}

// tools/serve_web.py --game DIR: the rip is offered under /game/.
let serverGame = null;
async function probeServerGame() {
  try {
    const r = await fetch('game/manifest.json', { cache: 'no-store' });
    if (r.ok) serverGame = (await r.json()).files;
  } catch {}
}

async function importFromServer() {
  const files = serverGame.map((f) => ({
    webkitRelativePath: 'game/' + f.path,
    size: f.size,
    slice: (a, b) => ({ arrayBuffer: async () =>
      (await fetch('game/' + encodeURI(f.path), { headers: { Range: `bytes=${a}-${b - 1}` } })).arrayBuffer() }),
    stream: () => ({ pipeTo: async (w) => {
      const r = await fetch('game/' + encodeURI(f.path));
      await r.body.pipeTo(w);
    } }),
  }));
  await importFolder(files);
}

$('pick').addEventListener('change', (e) => importFolder(e.target.files));
$('replace').onclick = () => {
  if (mod) { location.reload(); return; }
  $('pick').click();
};

// ---------------------------------------------------------------- input
const VK = {
  Enter: 0x0D, NumpadEnter: 0x0D, Space: 0x20, Backspace: 0x08, Tab: 0x09, Escape: 0x1B,
  ShiftLeft: 0x10, ShiftRight: 0x10, ControlLeft: 0x11, ControlRight: 0x11,
  AltLeft: 0x12, AltRight: 0x12, ArrowLeft: 0x25, ArrowUp: 0x26, ArrowRight: 0x27,
  ArrowDown: 0x28, Home: 0x24, End: 0x23, PageUp: 0x21, PageDown: 0x22, Insert: 0x2D,
  Delete: 0x2E, Comma: 0xBC, Period: 0xBE, Minus: 0xBD, Equal: 0xBB,
};
function vkOf(code) {
  if (VK[code]) return VK[code];
  let m;
  if ((m = /^Key([A-Z])$/.exec(code))) return m[1].charCodeAt(0);
  if ((m = /^Digit([0-9])$/.exec(code))) return 0x30 + +m[1];
  if ((m = /^Numpad([0-9])$/.exec(code))) return 0x60 + +m[1];
  if ((m = /^F([0-9]{1,2})$/.exec(code)) && +m[1] >= 1 && +m[1] <= 12) return 0x6F + +m[1];
  return 0;
}
function onKey(e, down) {
  if (!mod || e.ctrlKey && e.code !== 'ControlLeft' && e.code !== 'ControlRight') return;
  if (e.code === 'F11') return;                  // the browser's fullscreen
  const vk = vkOf(e.code);
  if (!vk) return;
  mod._web_key(vk, down ? 1 : 0);
  e.preventDefault();
}
addEventListener('keydown', (e) => onKey(e, true));
addEventListener('keyup', (e) => onKey(e, false));
addEventListener('blur', () => mod && mod._web_key(-1, 0));

// Standard mapping -> the bit order of the shim in xinput_device.c:
// A B X Y BACK GUIDE START LSTICK RSTICK LSHOULDER RSHOULDER DUP DDOWN DLEFT DRIGHT
const PAD_BITS = [0, 1, 2, 3, 9, 16, 8, 10, 11, 4, 5, 12, 13, 14, 15];
const lastPad = [];
function pollPads() {
  const pads = navigator.getGamepads ? navigator.getGamepads() : [];
  for (let port = 0; port < 4; port++) {
    const p = pads[port];
    if (!p || !p.connected) {
      if (lastPad[port]) { mod._web_pad(port, 0, 0, 0, 0, 0, 0, 0, 0); lastPad[port] = false; }
      continue;
    }
    lastPad[port] = true;
    let bits = 0;
    PAD_BITS.forEach((b, i) => { if (p.buttons[b] && p.buttons[b].pressed) bits |= 1 << i; });
    const ax = (i) => Math.max(-32767, Math.min(32767, Math.round((p.axes[i] || 0) * 32767)));
    const tr = (i) => Math.round((p.buttons[i] ? p.buttons[i].value : 0) * 32767);
    mod._web_pad(port, 1, bits >>> 0, ax(0), ax(1), ax(2), ax(3), tr(6), tr(7));
  }
}

// ---------------------------------------------------------------- video
let info = 0, image = null, lastSeq = -1, fpsT = performance.now(), fpsFlips = 0;
function frame() {
  requestAnimationFrame(frame);
  if (!mod) return;
  pollPads();
  const i32 = new Int32Array(mod.HEAPU8.buffer, info, 8);
  i32[6] = canvas.clientWidth; i32[7] = canvas.clientHeight;
  const ptr = mod._web_frame(info);
  const w = i32[0], h = i32[1], seq = i32[2];
  if (ptr && seq !== lastSeq) {
    lastSeq = seq;
    if (!image || image.width !== w || image.height !== h) {
      canvas.width = w; canvas.height = h;
      image = new ImageData(w, h);
    }
    // ImageData cannot view a SharedArrayBuffer: copy.
    image.data.set(new Uint8Array(mod.HEAPU8.buffer, ptr, w * h * 4));
    if (!ctx2d) ctx2d = canvas.getContext('2d', { alpha: false });
    ctx2d.putImageData(image, 0, 0);
  }
  if ((ptr || (gpu && gpu.frames)) && !overlay.hidden && !overlay.dataset.keep) overlay.hidden = true;
  const now = performance.now();
  if (now - fpsT >= 1000) {
    const flips = i32[3];
    stat.textContent = `${((flips - fpsFlips) * 1000 / (now - fpsT)).toFixed(1)} FPS · ${i32[4]} draws`;
    fpsFlips = flips; fpsT = now;
  }
}

// ---------------------------------------------------------------- audio
async function startAudio() {
  try {
    const ac = new AudioContext({ sampleRate: 48000, latencyHint: 'interactive' });
    await ac.audioWorklet.addModule('audio-worklet.js');
    const node = new AudioWorkletNode(ac, 'ring-player', { outputChannelCount: [2] });
    node.port.postMessage({ buffer: mod.HEAPU8.buffer, ring: mod._web_audio_ring() });
    node.connect(ac.destination);
    // Without a user gesture (autostart) resume() can wait forever.
    await Promise.race([ac.resume(), new Promise((r) => setTimeout(r, 1000))]);
    audio = { ctx: ac, node };
    $('mute').disabled = false;
  } catch (e) {
    log('[WEB] audio unavailable: ' + e);
  }
}
$('mute').onclick = async () => {
  if (!audio) return;
  if (audio.ctx.state === 'running') { await audio.ctx.suspend(); $('mute').textContent = 'Unmute'; }
  else { await audio.ctx.resume(); $('mute').textContent = 'Mute'; }
};
$('fs').onclick = () => $('stage').requestFullscreen && $('stage').requestFullscreen();

// ---------------------------------------------------------------- start
function envFromQuery() {
  // ?env=RECOMP_X=1&env=RECOMP_Y=2 -- the native build's knobs, for testing.
  const env = {};
  for (const kv of new URLSearchParams(location.search).getAll('env')) {
    const i = kv.indexOf('=');
    if (i > 0) env[kv.slice(0, i)] = kv.slice(i + 1);
  }
  return env;
}

if (new URLSearchParams(location.search).has('hb')) setInterval(() => log('[HB] ' + Math.round(performance.now())), 1000);

async function start() {
  say('Starting…', { progress: null });
  overlay.dataset.keep = '';
  const env = envFromQuery();
  // WebGPU unless ?gpu=sw; the software rasteriser is the fallback.
  if (params.get('gpu') !== 'sw' && env.RECOMP_GPU !== 'sw') {
    try {
      gpu = await WebGPURenderer.create(canvas, log);
    } catch (e) {
      log('[WEBGPU] unavailable: ' + (e.message || e));
      gpu = null;
    }
    if (gpu) env.RECOMP_WEBGPU = '1';
    else log('[WEBGPU] no adapter: software rasteriser');
  }
  mod = await SSX3({
    wgrAttach: (ctl) => (gpu ? gpu.attach(mod, ctl) : false),
    print: (t) => log(t),
    printErr: (t) => log(t),
    preRun: [(m) => { Object.assign(m.ENV, env); }],
    // The title ended (ExitProcess): a reboot -- the first boot formats the
    // emulated cache partition and relaunches -- or a quit to the dashboard.
    // Restart it; twice within 20 s would be a loop, so stop then.
    onGuestExit: (code) => {
      const last = +(sessionStorage.getItem('ssx3-exit') || 0);
      log(`[PAGE] title exited (${code}), restarting`);
      if (Date.now() - last < 20000) {
        say('<span class="bad">The game exited twice in a row.</span> See the log.');
        return;
      }
      sessionStorage.setItem('ssx3-exit', String(Date.now()));
      const q = new URLSearchParams(location.search);
      q.set('autostart', '');
      setTimeout(() => { location.search = q.toString(); }, 300);
    },
  });
  info = mod._malloc(32);
  new Int32Array(mod.HEAPU8.buffer, info, 8).fill(0);
  await startAudio();
  $('fs').disabled = false;
  delete overlay.dataset.keep;
  say('Booting… (the first frame takes a few seconds)');
  mod.callMain([]);
}

function ready() {
  say('Game files found in this browser. Sound needs a click to start.',
      { buttons: [button('Play', start, true)] });
}

async function boot() {
  if (!crossOriginIsolated) {
    say('<span class="bad">This page must be served cross-origin isolated</span> '
      + '(COOP <code>same-origin</code> + COEP <code>require-corp</code>), or threads cannot share memory. '
      + 'Use <code>tools/serve_web.py</code>.');
    return;
  }
  if (!navigator.storage || !navigator.storage.getDirectory) {
    say('<span class="bad">This browser has no origin-private file system.</span> Use a current Chrome, Edge or Firefox.');
    return;
  }
  requestAnimationFrame(frame);
  await probeServerGame();
  if (params.has('fresh')) {
    try { await (await opfsDir(['ssx3'])).removeEntry('game', { recursive: true }); } catch {}
  }
  if (await haveGame()) {
    if (params.has('autostart')) start(); else ready();
  } else if (serverGame && params.get('import') === 'server') importFromServer();
  else say('Choose your SSX 3 game folder: the one with <strong>default.xbe</strong> and the '
         + '<strong>data</strong> folder, from your own disc. It is copied into this browser '
         + '(about 2.2 GB) once and stays on this computer.', { buttons: pickButtons() });
}
const params = new URLSearchParams(location.search);
boot();
