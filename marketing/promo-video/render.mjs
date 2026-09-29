import { chromium } from '@playwright/test';
import fs from 'fs';
const [mode, ...rest] = process.argv.slice(2);
const FPS = 30, DUR = 88;
const browser = await chromium.launch({ args: ['--font-render-hinting=none'] });
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
  const W = 4; fs.mkdirSync('frames', { recursive: true });
  const N = FPS * DUR;
  await Promise.all(Array.from({ length: W }, async (_, w) => {
    const p = await mk();
    for (let f = w; f < N; f += W) {
      await p.evaluate(t => render(t), f / FPS);
      await p.screenshot({ path: `frames/${String(f).padStart(5, '0')}.jpg`, type: 'jpeg', quality: 94 });
    }
    if (p.errs.length) console.log('errors', p.errs.slice(0, 3));
  }));
}
await browser.close();
