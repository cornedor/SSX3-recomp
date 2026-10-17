#!/usr/bin/env node
// Headless Playwright run of the web build: serves build-web/ with the rip
// (tools/serve_web.py --game), opens it in headless Chromium with a persistent
// profile (work/pw-profile: the game files are copied into OPFS only once),
// writes the console to work/web-run/log.txt and a screenshot of the game
// canvas every --every seconds, and kills everything after --secs.
//
//   source scripts/web-env.sh
//   (cd work/pw && npm install playwright@1.48.2 && npx playwright install chromium)
//   node tools/web_test.cjs --secs 120 --every 10 --query "&env=RECOMP_AUTONAV=race"
'use strict';
const path = require('path'), fs = require('fs'), { spawn } = require('child_process');
const root = path.resolve(__dirname, '..');
const { chromium } = require(path.join(root, 'work/pw/node_modules/playwright'));

const arg = (n, d) => { const i = process.argv.indexOf('--' + n); return i > 0 ? process.argv[i + 1] : d; };
const secs = +arg('secs', 90), every = +arg('every', 10), query = arg('query', '');
const port = +arg('port', 8093), out = path.resolve(arg('out', path.join(root, 'work/web-run')));
const keys = arg('keys', '');   // "t:Key,t:Key" -- press Key (KeyboardEvent.key) at t seconds

(async () => {
  fs.rmSync(out, { recursive: true, force: true });
  fs.mkdirSync(out, { recursive: true });
  const logf = fs.openSync(path.join(out, 'log.txt'), 'w');
  const t0 = Date.now();
  const w = (s) => fs.writeSync(logf, `${((Date.now() - t0) / 1000).toFixed(1).padStart(6)} ${s}\n`);
  const srv = spawn('python3', [path.join(root, 'tools/serve_web.py'), path.join(root, 'build-web'),
    '--port', String(port), '--game', path.join(root, 'disk/SSX3')], { stdio: 'ignore' });
  await new Promise((r) => setTimeout(r, 800));
  const ctx = await chromium.launchPersistentContext(path.join(root, 'work/pw-profile'), {
    // Full Chromium in new-headless mode: unlike the headless shell it has a
    // WebGPU adapter on the real GPU (Vulkan).
    // PW_CHROMIUM=/usr/bin/chromium-browser: a system build (aarch64 Linux,
    // where Playwright ships none).
    headless: true, channel: 'chromium', viewport: { width: 1000, height: 900 },
    executablePath: process.env.PW_CHROMIUM || undefined,
    // --video: record the whole run (out/video/*.webm; [TEST] stage box logs the
    // game area for cropping).
    recordVideo: process.argv.includes('--video')
      ? { dir: path.join(out, 'video'), size: { width: 1000, height: 900 } } : undefined,
    args: ['--autoplay-policy=no-user-gesture-required', '--enable-unsafe-webgpu',
           '--enable-features=Vulkan', '--use-angle=vulkan',
           ...(process.env.PW_ARGS || '').split(' ').filter(Boolean)],
  });
  let shot = 0;
  const finish = async (why) => {
    w(`[TEST] end: ${why}`);
    try { await ctx.close(); } catch {}
    srv.kill('SIGKILL');
    fs.closeSync(logf);
    console.log(`${why}; ${shot} shots and log.txt in ${out}`);
    process.exit(0);
  };
  const page = ctx.pages()[0] || await ctx.newPage();
  page.on('console', (m) => w(m.text()));
  // Workers' own console (pthreads print through the page thread, so this
  // only shows what bypasses it -- useful when the page thread is stuck).
  if (process.argv.includes('--workers'))
    page.on('worker', (wk) => wk.on('console', (m) => w('[W] ' + m.text())));
  // --worker-errors: uncaught exceptions in workers (pthreads) with their
  // stacks, through CDP (the page only hears "worker sent an error!").
  if (process.argv.includes('--worker-errors')) {
    const cdp = await ctx.newCDPSession(page);
    let id = 1;
    const send = (sid, method, params = {}) => cdp.send('Target.sendMessageToTarget',
      { sessionId: sid, message: JSON.stringify({ id: id++, method, params }) }).catch(() => {});
    cdp.on('Target.attachedToTarget', (e) => {
      send(e.sessionId, 'Runtime.enable');
      send(e.sessionId, 'Runtime.runIfWaitingForDebugger');
    });
    cdp.on('Target.receivedMessageFromTarget', (e) => {
      const m = JSON.parse(e.message);
      if (m.method !== 'Runtime.exceptionThrown') return;
      const d = m.params.exceptionDetails;
      w('[WORKER-EXC] ' + (d.exception && d.exception.description || d.text));
      for (const f of (d.stackTrace && d.stackTrace.callFrames || []).slice(0, 30))
        w(`[WORKER-EXC]   ${f.functionName || '(anon)'} ${f.url.split('/').pop()}:${f.lineNumber + 1}:${f.columnNumber}`);
    });
    await cdp.send('Target.setAutoAttach', { autoAttach: true, waitForDebuggerOnStart: false, flatten: false });
  }
  page.on('pageerror', (e) => w('[PAGEERROR] ' + e.message + '\n' + (e.stack || '')));
  page.on('crash', () => finish('page crashed'));
  const url = `http://127.0.0.1:${port}/?import=server&autostart${query}`;
  w('[TEST] ' + url);
  await page.goto(url);
  if (process.argv.includes('--video')) {
    const b = await page.locator('#stage').boundingBox();
    if (b) w(`[TEST] stage box ${Math.round(b.x)} ${Math.round(b.y)} ${Math.round(b.width)} ${Math.round(b.height)}`);
  }
  // --pause-at S: pause the page thread in the debugger after S seconds and
  // log its JS stack (a hang on the page thread stops the log and the shots).
  const pauseAt = +arg('pause-at', 0);
  if (pauseAt) {
    const cdp = await ctx.newCDPSession(page);
    await cdp.send('Debugger.enable');
    cdp.on('Debugger.paused', async (e) => {
      w('[TEST] page thread paused: ' + e.reason);
      for (const f of e.callFrames.slice(0, 15))
        w(`[TEST]   ${f.functionName || '(anon)'} ${f.url.split('/').pop()}:${f.location.lineNumber + 1}`);
      await cdp.send('Debugger.resume').catch(() => {});
    });
    setTimeout(() => cdp.send('Debugger.pause').catch((e) => w('[TEST] pause: ' + e.message)), pauseAt * 1000);
  }
  for (const k of keys.split(',').filter(Boolean)) {
    const [t, key] = k.split(':');
    setTimeout(() => page.keyboard.press(key, { delay: 120 }).catch(() => {}), +t * 1000);
  }
  const timer = setInterval(async () => {
    try {
      const f = path.join(out, `shot_${String(++shot).padStart(3, '0')}_${Math.round((Date.now() - t0) / 1000)}s.png`);
      await page.locator('#stage').screenshot({ path: f });
    } catch (e) { w('[TEST] screenshot failed: ' + e.message); }
  }, every * 1000);
  setTimeout(() => { clearInterval(timer); finish(`${secs} s`); }, secs * 1000);
})();
