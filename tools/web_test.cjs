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
    headless: true, viewport: { width: 1000, height: 900 },
    args: ['--autoplay-policy=no-user-gesture-required', '--enable-unsafe-swiftshader'],
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
  page.on('pageerror', (e) => w('[PAGEERROR] ' + e.message + '\n' + (e.stack || '')));
  page.on('crash', () => finish('page crashed'));
  const url = `http://127.0.0.1:${port}/?import=server&autostart${query}`;
  w('[TEST] ' + url);
  await page.goto(url);
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
