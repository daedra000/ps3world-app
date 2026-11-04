/*
make-flags.mjs: the language screen's flags, from flag-icons (MIT, github.com/lipis/flag-icons).

  node tools/make-flags.mjs "<flag-icons folder>"

  data/flags/<code>.png   120x90 (4:3), for the flags src/lang.c names
  data/licenses/flag-icons.txt

Uses sharp from generator/node_modules (cd generator && npm install first).
*/
import fs from 'node:fs';
import path from 'node:path';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';

const ROOT = path.join(path.dirname(fileURLToPath(import.meta.url)), '..');
const sharp = createRequire(path.join(ROOT, 'generator/'))('sharp');
const src = process.argv[2];
if (!src) { console.error('usage: node tools/make-flags.mjs "<flag-icons folder>"'); process.exit(1); }

// lang.c's flags[], in its order
const lang = fs.readFileSync(path.join(ROOT, 'src/lang.c'), 'utf8');
const codes = [...lang.match(/flags\[LANG_COUNT\] = \{([^}]*)\}/)[1].matchAll(/"([a-z]+)"/g)].map(m => m[1]);
fs.mkdirSync(path.join(ROOT, 'data/flags'), { recursive: true });
for (const c of codes) {
  const svg = path.join(src, 'flags/4x3', `${c}.svg`);
  await sharp(svg, { density: 300 }).resize(120, 90, { fit: 'fill' }).png().toFile(path.join(ROOT, 'data/flags', `${c}.png`));
}
fs.copyFileSync(path.join(src, 'LICENSE'), path.join(ROOT, 'data/licenses/flag-icons.txt'));
console.log(`data/flags: ${codes.join(' ')}`);
