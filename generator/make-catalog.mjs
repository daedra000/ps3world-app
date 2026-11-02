/*
make-catalog.mjs: builds catalog/v1/catalog.json and its icons for the PS3 World app.

  node make-catalog.mjs                  everything
  node make-catalog.mjs --limit 3        the first 3 of each type (a quick sample)
  node make-catalog.mjs --only theme     one type (homebrew, theme, avatar; comma separated)
  node make-catalog.mjs --verify         re-hash every file in the written catalog.json

Reads the site build data (themes.json, avatars.json) and homebrew.manifest.json,
hashes the local copy of every download (the same files that are on R2), and
writes into OUT_DIR. It never uploads: it prints the rclone command to run.
Hashes are cached by path, size and modified time, so a second run is quick.
*/
import fs from 'node:fs';
import fsp from 'node:fs/promises';
import path from 'node:path';
import crypto from 'node:crypto';
import { fileURLToPath } from 'node:url';
import sharp from 'sharp';

const HERE = path.dirname(fileURLToPath(import.meta.url));

// ---- where things are on this PC ----
const CONFIG = {
  themesJson: 'G:/ps3-themes/_site/data/themes.json',
  avatarsJson: 'G:/PS3 Avatars/_site_avatars/data/avatars.json',
  manifest: path.join(HERE, 'homebrew.manifest.json'),
  homebrewRoot: 'D:/PS3 WORLD R2',				// manifest "file" paths are relative to this (and to the bucket)
  siteRoot: 'G:/PS3 World Self Host Ver - Copy',	// manifest "icon" paths are relative to this
  // a dl.ps3.world path's first folder -> where those files are on this PC
  roots: {
    bestps3themes: 'G:/ps3-themes/Best-PS3-Themes/PS3_Themes',
    ps3themes: 'G:/ps3-themes/PS3_Themes',
    ps3avatars: 'D:/PS3 Avatars',
  },
  outDir: 'G:/ps3world-catalog',
  baseUrl: 'https://dl.ps3.world/',
  bucket: 'R2:ps3wrld-content',
  includeNsfw: false,
};

const SCHEMA = 1;
const CATALOG_DIR = 'catalog/v1';			// on dl.ps3.world and under outDir
const ICON_SIZE = 128;
const ICON_BG = { r: 0x1a, g: 0x1c, b: 0x20 };
const HASH_JOBS = 4;

// ---- arguments ----
const args = process.argv.slice(2);
const argVal = name => { const i = args.indexOf(name); return i >= 0 ? args[i + 1] : undefined; };
const LIMIT = argVal('--limit') ? parseInt(argVal('--limit'), 10) : 0;
const ONLY = (argVal('--only') || 'homebrew,theme,avatar').split(',');
const VERIFY = args.includes('--verify');

const warnings = [];
const warn = msg => { warnings.push(msg); };

// ---- paths ----

// "bestps3themes/a%20b/c.p3t" -> local file, via CONFIG.roots
function localFor(relUrl) {
  const parts = relUrl.split('/').map(s => decodeURIComponent(s));
  const root = CONFIG.roots[parts[0]];
  return root ? path.join(root, ...parts.slice(1)) : null;
}

function relFromUrl(url) {
  if (!url || !url.startsWith(CONFIG.baseUrl)) return null;
  return url.slice(CONFIG.baseUrl.length);
}

const encodePath = p => p.split('/').map(encodeURIComponent).join('/');

// the site builds write JSON from PowerShell, with a byte-order mark
const readJson = file => JSON.parse(fs.readFileSync(file, 'utf8').replace(/^\uFEFF/, ''));

// ---- hashing, cached ----
const cachePath = path.join(CONFIG.outDir, '.cache', 'hashes.json');
let hashCache = {};
try { hashCache = JSON.parse(fs.readFileSync(cachePath, 'utf8')); } catch {}

async function fileInfo(file, useCache = true) {
  let st;
  try { st = await fsp.stat(file); } catch { return null; }
  const key = `${file}|${st.size}|${st.mtimeMs}`;
  if (useCache && hashCache[key]) return { size: st.size, sha256: hashCache[key] };
  const sha256 = await new Promise((resolve, reject) => {
    const h = crypto.createHash('sha256');
    fs.createReadStream(file).on('data', d => h.update(d)).on('end', () => resolve(h.digest('hex'))).on('error', reject);
  });
  hashCache[key] = sha256;
  return { size: st.size, sha256 };
}

// runs fn over items, n at a time, with a progress line
async function pool(items, n, label, fn) {
  let next = 0, done = 0;
  const tick = () => { if (process.stdout.isTTY) process.stdout.write(`\r${label}: ${done}/${items.length}`); };
  await Promise.all(Array.from({ length: n }, async () => {
    while (next < items.length) {
      const i = next++;
      await fn(items[i]);
      done++;
      if (done % 50 === 0) tick();
    }
  }));
  tick();
  if (process.stdout.isTTY) process.stdout.write('\n');
}

// ---- icons ----
async function makeIcon(src, id) {
  if (!src) return '';
  try { await fsp.access(src); } catch { warn(`icon missing for ${id}: ${src}`); return ''; }
  const rel = `${CATALOG_DIR}/icons/${id}.jpg`;
  const out = path.join(CONFIG.outDir, rel);
  try {
    const [a, b] = await Promise.all([fsp.stat(src), fsp.stat(out).catch(() => null)]);
    if (b && b.mtimeMs >= a.mtimeMs) return rel;		// made already
    // the PS3 shows JPEG and PNG only: baseline JPEG, letterboxed into a square
    // (failOn none: a few avatar PNGs have damaged chunks but still decode)
    await sharp(src, { failOn: 'none' }).resize(ICON_SIZE, ICON_SIZE, { fit: 'contain', background: ICON_BG })
      .flatten({ background: ICON_BG })
      .jpeg({ quality: 80, progressive: false, mozjpeg: false, chromaSubsampling: '4:2:0' })
      .toFile(out);
    return rel;
  } catch (e) {
    warn(`icon failed for ${id}: ${e.message}`);
    return '';
  }
}

// ---- ids ----
const usedIds = new Set();
function uniqueId(base) {
  const clean = base.toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-|-$/g, '').slice(0, 60);
  let id = clean, n = 2;
  while (usedIds.has(id)) id = `${clean}-${n++}`;
  usedIds.add(id);
  return id;
}

// ---- sources: each returns { item fields..., _file, _icon } ----
function homebrewSources() {
  const m = readJson(CONFIG.manifest);
  return m.items.map(e => ({
    id: uniqueId('hb-' + path.basename(e.file, '.pkg')),
    type: 'homebrew',
    title: e.title,
    author: e.author || '',
    version: e.version || '',
    description: e.description || '',
    group: e.group || '',
    url: encodePath(e.file),
    requires: e.requires || 'hen',
    _file: path.join(CONFIG.homebrewRoot, e.file),
    _icon: e.icon ? path.join(CONFIG.siteRoot, e.icon) : '',
  }));
}

function themeSources() {
  const list = readJson(CONFIG.themesJson);
  const out = [];
  for (const t of list) {
    if (t.nsfw && !CONFIG.includeNsfw) continue;
    const rel = relFromUrl(t.p3t);
    if (!rel) { warn(`theme ${t.slug}: download isn't on ${CONFIG.baseUrl}`); continue; }
    const img = relFromUrl(t.image);
    out.push({
      id: uniqueId(`th-${t.source}-${t.slug}`),
      type: 'theme',
      title: t.title,
      author: t.author || '',
      version: '',
      description: `${t.sourceName} · ${(t.categories || [t.category]).join(', ')}`,
      group: t.category || '',
      url: rel,
      requires: 'any',
      _file: localFor(rel),
      _icon: img && t.hasImage ? localFor(img) : '',
    });
  }
  return out;
}

function avatarSources() {
  const list = readJson(CONFIG.avatarsJson);
  const perPack = {};
  const out = [];
  for (const a of list) {
    const rel = relFromUrl(a.dl);
    if (!rel) { warn(`avatar ${a.pack}/${a.title}: no download on ${CONFIG.baseUrl}`); continue; }
    const n = perPack[a.pack] = (perPack[a.pack] || 0) + 1;
    const img = relFromUrl(a.image);
    out.push({
      id: uniqueId(`av-${path.basename(decodeURIComponent(rel), '.edat').replace(/^PSNA_/, '').slice(0, 16)}`),
      type: 'avatar',
      title: `${a.packName} #${n}`,
      author: '',
      version: '',
      description: `Avatar ${a.title} from ${a.packName} (${[a.region, a.titleId].filter(Boolean).join(', ')})`,
      group: a.packName,
      url: rel,
      requires: 'hen',
      _file: localFor(rel),
      _icon: img ? localFor(img) : '',
    });
  }
  return out;
}

// ---- build ----
async function build() {
  await fsp.mkdir(path.join(CONFIG.outDir, CATALOG_DIR, 'icons'), { recursive: true });
  await fsp.mkdir(path.dirname(cachePath), { recursive: true });

  let sources = [];
  const pick = list => (LIMIT ? list.slice(0, LIMIT) : list);
  if (ONLY.includes('homebrew')) sources.push(...pick(homebrewSources()));
  if (ONLY.includes('theme')) sources.push(...pick(themeSources()));
  if (ONLY.includes('avatar')) sources.push(...pick(avatarSources()));

  const items = [];
  await pool(sources, HASH_JOBS, 'hashing', async s => {
    const info = s._file && await fileInfo(s._file);
    if (!info) { warn(`${s.type} ${s.id}: file not found: ${s._file}`); return; }
    s.size = info.size;
    s.sha256 = info.sha256;
    s.icon = await makeIcon(s._icon, s.id);
    items.push(s);
  });
  fs.writeFileSync(cachePath, JSON.stringify(hashCache));

  const order = { homebrew: 0, theme: 1, avatar: 2 };
  items.sort((a, b) => order[a.type] - order[b.type] || a.title.localeCompare(b.title, 'en', { sensitivity: 'base' }) || a.id.localeCompare(b.id));
  const catalog = {
    schema: SCHEMA,
    generated: new Date().toISOString().replace(/\.\d+Z$/, 'Z'),
    base_url: CONFIG.baseUrl,
    items: items.map(s => ({
      id: s.id, type: s.type, title: s.title, author: s.author, version: s.version,
      description: s.description, group: s.group, size: s.size, sha256: s.sha256,
      url: s.url, icon: s.icon, requires: s.requires,
    })),
  };
  const json = JSON.stringify(catalog);
  const outFile = path.join(CONFIG.outDir, CATALOG_DIR, 'catalog.json');
  fs.writeFileSync(outFile, json);

  const count = t => items.filter(i => i.type === t).length;
  console.log(`\nwrote ${outFile}`);
  console.log(`  ${items.length} items: ${count('homebrew')} homebrew, ${count('theme')} themes, ${count('avatar')} avatars`);
  console.log(`  ${(json.length / 1024).toFixed(1)} KB, ${items.filter(i => !i.icon).length} without an icon`);
  report();
  console.log('\nUpload (you run this; copy never deletes anything on R2):');
  console.log(`  rclone copy "${path.join(CONFIG.outDir, 'catalog').replace(/\//g, '\\')}" ${CONFIG.bucket}/catalog --progress`);
}

// ---- verify: every file in catalog.json against its sha256, read fresh ----
async function verify() {
  const file = path.join(CONFIG.outDir, CATALOG_DIR, 'catalog.json');
  const cat = readJson(file);
  const homebrewByUrl = Object.fromEntries(homebrewSources().map(s => [s.url, s._file]));
  let ok = 0, bad = 0;
  await pool(cat.items, HASH_JOBS, 'verifying', async it => {
    const local = it.type === 'homebrew' ? homebrewByUrl[it.url] : localFor(it.url);
    const info = local && await fileInfo(local, false);
    if (info && info.sha256 === it.sha256 && info.size === it.size) { ok++; return; }
    bad++;
    warn(`MISMATCH ${it.id}: ${local} ${info ? info.sha256 + ' ' + info.size : 'missing'}`);
  });
  console.log(`\nverified ${cat.items.length} items: ${ok} match, ${bad} don't`);
  report();
  process.exitCode = bad ? 1 : 0;
}

function report() {
  if (!warnings.length) return;
  console.log(`\n${warnings.length} warning(s):`);
  for (const w of warnings.slice(0, 40)) console.log('  ' + w);
  if (warnings.length > 40) console.log(`  ... and ${warnings.length - 40} more`);
}

(VERIFY ? verify() : build()).catch(e => { console.error(e); process.exit(1); });
