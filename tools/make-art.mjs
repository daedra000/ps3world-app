/*
make-art.mjs: the XMB pictures and the in-app background, from the site.

  node tools/make-art.mjs "G:\PS3 World Self Host Ver - Copy"

  art/ICON0.PNG     320x176, the site's meta image (files/meta.jpg)
  art/PIC1.PNG      1920x1080, the site's index-page waves
  data/backdrop.png 1280x720, the same waves for the app's screens
  data/logo.png     the site's wordmark (files/PS3-WORLD-logo.png)

Uses sharp from generator/node_modules (cd generator && npm install first).
*/
import path from 'node:path';
import fs from 'node:fs';
import { fileURLToPath } from 'node:url';
import { createRequire } from 'node:module';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const sharp = createRequire(path.join(ROOT, 'generator/'))('sharp');
const site = process.argv[2];
if (!site) { console.error('usage: node tools/make-art.mjs <main site folder>'); process.exit(1); }

// the index page's .page-waves: three blurred gradient waves over --bg, a glow and a fade
function wavesSvg(w, h) {
  const c = { a: '#0070d1', b: '#2b8fff', c: '#4ad9ff', d: '#1a3a7a', e: '#0a1a3a', f: '#6ad9ff' };
  const waveH = h * 0.578, waveY = h - waveH;
  const wave = (id, stops, d, blur) => `
    <svg x="0" y="${waveY}" width="${w}" height="${waveH}" viewBox="0 380 1440 520" preserveAspectRatio="none" opacity="0.26">
      <defs>
        <linearGradient id="g${id}" x1="0" x2="1" y1="0" y2="0">
          ${stops.map(([o, col, op]) => `<stop offset="${o}%" stop-color="${col}" stop-opacity="${op}"/>`).join('')}
        </linearGradient>
        ${blur ? `<filter id="b${id}" x="-10%" y="-10%" width="120%" height="120%"><feGaussianBlur stdDeviation="6"/></filter>` : ''}
      </defs>
      <path d="${d}" fill="url(#g${id})" ${blur ? `filter="url(#b${id})"` : ''}/>
    </svg>`;
  return `<svg xmlns="http://www.w3.org/2000/svg" width="${w}" height="${h}">
    <defs>
      <linearGradient id="bg" x1="0" x2="0" y1="0" y2="1"><stop offset="0" stop-color="#05070d"/><stop offset="1" stop-color="#0a0e18"/></linearGradient>
      <radialGradient id="glow" cx="0.8" cy="0.5" r="0.6">
        <stop offset="0" stop-color="#2b8fff" stop-opacity="0.18"/><stop offset="0.6" stop-color="#0070d1" stop-opacity="0.06"/><stop offset="1" stop-color="#0070d1" stop-opacity="0"/>
      </radialGradient>
      <linearGradient id="fade" x1="0" x2="0" y1="0" y2="1">
        <stop offset="0" stop-color="#05070d" stop-opacity="0.12"/><stop offset="0.22" stop-color="#05070d" stop-opacity="0"/>
        <stop offset="0.4" stop-color="#05070d" stop-opacity="0"/><stop offset="0.55" stop-color="#05070d" stop-opacity="0.12"/>
        <stop offset="0.85" stop-color="#05070d" stop-opacity="0.3"/><stop offset="1" stop-color="#05070d" stop-opacity="0.35"/>
      </linearGradient>
    </defs>
    <rect width="100%" height="100%" fill="url(#bg)"/>
    <rect width="100%" height="100%" fill="url(#glow)" opacity="0.35"/>
    ${wave(3, [[0, c.e, 0], [45, c.c, 0.25], [75, c.f, 0.4], [100, c.f, 0]], 'M0,720 C180,600 360,780 540,660 C720,540 900,760 1080,640 C1260,520 1380,700 1440,660 L1440,1400 L0,1400 Z', true)}
    ${wave(2, [[0, c.d, 0], [45, c.a, 0.4], [75, c.b, 0.6], [100, c.b, 0]], 'M0,600 C180,500 360,660 540,560 C720,460 900,660 1080,560 C1260,460 1380,620 1440,580 L1440,1400 L0,1400 Z', true)}
    ${wave(1, [[0, c.a, 0], [45, c.b, 0.5], [75, c.c, 0.7], [100, c.c, 0]], 'M0,500 C180,420 360,560 540,470 C720,380 900,560 1080,470 C1260,380 1380,530 1440,490 L1440,1400 L0,1400 Z', false)}
    <rect width="100%" height="100%" fill="url(#fade)"/>
  </svg>`;
}

fs.mkdirSync(path.join(ROOT, 'art'), { recursive: true });
fs.mkdirSync(path.join(ROOT, 'data'), { recursive: true });
await sharp(path.join(site, 'files/meta.jpg')).resize(320, 176, { fit: 'cover' }).png().toFile(path.join(ROOT, 'art/ICON0.PNG'));
await sharp(Buffer.from(wavesSvg(1920, 1080))).png().toFile(path.join(ROOT, 'art/PIC1.PNG'));
await sharp(Buffer.from(wavesSvg(1280, 720))).png().toFile(path.join(ROOT, 'data/backdrop.png'));
fs.copyFileSync(path.join(site, 'files/PS3-WORLD-logo.png'), path.join(ROOT, 'data/logo.png'));
console.log('art/ICON0.PNG, art/PIC1.PNG, data/backdrop.png, data/logo.png');
