// a QR code for a short text: byte mode, version 2 (25x25), error correction M, mask 0
// (after Project Nayuki's reference QR generator). node tools/qr.mjs https://ps3.world/contact data/qr-contact.png 12
import { createRequire } from 'node:module';
const sharp = createRequire(new URL('../generator/', import.meta.url))('sharp');
const [text, out, px = '12'] = process.argv.slice(2);
const VER = 2, SIZE = 17 + 4 * VER, DATA_CW = 28, EC_CW = 16;		// version 2-M: one block, 28 data + 16 ec

// ---- the data codewords ----
const bytes = [...Buffer.from(text, 'utf8')];
if (bytes.length > 26) throw new Error('too long for version 2-M (26 bytes)');
const bits = [];
const put = (v, n) => { for (let i = n - 1; i >= 0; i--) bits.push((v >>> i) & 1); };
put(0b0100, 4);						// byte mode
put(bytes.length, 8);
for (const b of bytes) put(b, 8);
put(0, Math.min(4, DATA_CW * 8 - bits.length));		// terminator
while (bits.length % 8) bits.push(0);
const data = [];
for (let i = 0; i < bits.length; i += 8) data.push(bits.slice(i, i + 8).reduce((a, b) => a * 2 + b, 0));
for (let pad = 0xec; data.length < DATA_CW; pad ^= 0xec ^ 0x11) data.push(pad);

// ---- Reed-Solomon over GF(256), 0x11D ----
const mul = (x, y) => { let z = 0; for (let i = 7; i >= 0; i--) { z = (z << 1) ^ ((z >>> 7) * 0x11d); z ^= ((y >>> i) & 1) * x; } return z; };
const divisor = (() => { const r = new Array(EC_CW).fill(0); r[EC_CW - 1] = 1; let root = 1;
  for (let i = 0; i < EC_CW; i++) { for (let j = 0; j < r.length; j++) { r[j] = mul(r[j], root); if (j + 1 < r.length) r[j] ^= r[j + 1]; } root = mul(root, 2); } return r; })();
const ec = (() => { const r = divisor.map(() => 0);
  for (const b of data) { const f = b ^ r.shift(); r.push(0); divisor.forEach((c, i) => { r[i] ^= mul(c, f); }); } return r; })();
const codewords = [...data, ...ec];

// ---- the matrix: [y][x] ----
const mod = Array.from({ length: SIZE }, () => new Array(SIZE).fill(false));
const fn = Array.from({ length: SIZE }, () => new Array(SIZE).fill(false));
const set = (x, y, dark) => { mod[y][x] = dark; fn[y][x] = true; };
for (let i = 0; i < SIZE; i++) { set(6, i, i % 2 === 0); set(i, 6, i % 2 === 0); }		// timing
const finder = (cx, cy) => { for (let dy = -4; dy <= 4; dy++) for (let dx = -4; dx <= 4; dx++) {
  const d = Math.max(Math.abs(dx), Math.abs(dy)), x = cx + dx, y = cy + dy;
  if (x >= 0 && x < SIZE && y >= 0 && y < SIZE) set(x, y, d !== 2 && d !== 4); } };
finder(3, 3); finder(SIZE - 4, 3); finder(3, SIZE - 4);
for (let dy = -2; dy <= 2; dy++) for (let dx = -2; dx <= 2; dx++) set(18 + dx, 18 + dy, Math.max(Math.abs(dx), Math.abs(dy)) !== 1);	// alignment (v2)
// format: level M (0), mask 0
const fdata = (0 << 3) | 0; let rem = fdata;
for (let i = 0; i < 10; i++) rem = (rem << 1) ^ ((rem >>> 9) * 0x537);
const fbits = ((fdata << 10) | rem) ^ 0x5412, fb = i => ((fbits >>> i) & 1) === 1;
for (let i = 0; i <= 5; i++) set(8, i, fb(i));
set(8, 7, fb(6)); set(8, 8, fb(7)); set(7, 8, fb(8));
for (let i = 9; i < 15; i++) set(14 - i, 8, fb(i));
for (let i = 0; i < 8; i++) set(SIZE - 1 - i, 8, fb(i));
for (let i = 8; i < 15; i++) set(8, SIZE - 15 + i, fb(i));
set(8, SIZE - 8, true);				// the dark module

// data, in the zigzag; then mask 0 ((x + y) even flips) on all but the function modules
let k = 0;
for (let right = SIZE - 1; right >= 1; right -= 2) {
  if (right === 6) right = 5;
  for (let v = 0; v < SIZE; v++) for (let j = 0; j < 2; j++) {
    const x = right - j, up = ((right + 1) & 2) === 0, y = up ? SIZE - 1 - v : v;
    if (!fn[y][x] && k < codewords.length * 8) { mod[y][x] = ((codewords[k >>> 3] >>> (7 - (k & 7))) & 1) === 1; k++; }
  }
}
for (let y = 0; y < SIZE; y++) for (let x = 0; x < SIZE; x++) if (!fn[y][x] && (x + y) % 2 === 0) mod[y][x] = !mod[y][x];

// ---- the picture: black on white, a 4-module quiet zone ----
const P = +px, Q = 4, W = (SIZE + 2 * Q) * P;
let rects = '';
for (let y = 0; y < SIZE; y++) for (let x = 0; x < SIZE; x++) if (mod[y][x]) rects += `<rect x="${(x + Q) * P}" y="${(y + Q) * P}" width="${P}" height="${P}"/>`;
await sharp(Buffer.from(`<svg xmlns="http://www.w3.org/2000/svg" width="${W}" height="${W}" shape-rendering="crispEdges"><rect width="${W}" height="${W}" fill="#fff"/><g fill="#000">${rects}</g></svg>`)).png().toFile(out);
console.log(`${out}: ${SIZE}x${SIZE} modules, ${W}x${W} px, "${text}" (${bytes.length} bytes)`);
