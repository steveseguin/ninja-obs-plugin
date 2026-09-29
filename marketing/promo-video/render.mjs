import { chromium } from '@playwright/test';
import fs from 'fs';
const [mode, ...rest] = process.argv.slice(2);
const FPS = 30, DUR = 88;
const browser = await chromium.launch({ args: ['--font-render-hinting=none'], executablePath: process.env.CHROMIUM_PATH || undefined });
async function mk() {
  const p = await browser.newPage({ viewport: { width: 1920, height: 1080 } });
  await p.goto('file://' + process.cwd() + '/index.html');
  await p.evaluate(() => document.fonts.ready);
  const errs = []; p.on('pageerror', e => errs.push(e.message)); p.errs = errs;
  return p;
}
if (mode === 'preview') {
  const p = await mk();
  fs.mkdirSync('preview', { recursive: true });
  for (const t of rest) { await p.evaluate(t => render(t), Number(t)); await p.screenshot({ path: `preview/t${t}.jpg`, quality: 70, type: 'jpeg' }); }
  console.log('errors', p.errs);
} else {
  // "short": hard-cut highlight reel built from these [start, end) windows of the full timeline
  const SEGMENTS = mode === 'short' ? [[6, 12], [20, 32], [32, 40], [48, 56], [80, 88]] : [[0, DUR]];
  const dir = mode === 'short' ? 'frames-short' : 'frames';
  const times = SEGMENTS.flatMap(([a, b]) => Array.from({ length: (b - a) * FPS }, (_, i) => a + i / FPS));
  fs.mkdirSync(dir, { recursive: true });
  const W = 4, N = times.length;
  await Promise.all(Array.from({ length: W }, async (_, w) => {
    const p = await mk();
    for (let f = w; f < N; f += W) {
      await p.evaluate(([t, hard]) => { window.HARD_CUTS = hard; render(t); }, [times[f], mode === 'short']);
      await p.screenshot({ path: `${dir}/${String(f).padStart(5, '0')}.jpg`, type: 'jpeg', quality: 94 });
    }
    if (p.errs.length) console.log('errors', p.errs.slice(0, 3));
  }));
}
await browser.close();
