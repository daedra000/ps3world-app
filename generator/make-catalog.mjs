/*
make-catalog.mjs: builds the PS3 World app's catalogue under catalog/v1/:

  catalog.json                  the index: each section's lists, their pages (with
                                sha256s), counts and where each letter starts
  homebrew/<category>/N.json    homebrew, one list per site category, 100 items a page
  theme/<list>/N.json           themes: all of them, then one list per site category
  avatar/<list>/N.json          avatars: all of them, then one list per region, as packs (folders):
  avatar/<list>/packs/<pack>/   each pack's avatars, a list of its own (its item's "list")
  <list>/search.txt             "title<TAB>author<TAB>group" a line, for searching a list
  icons/<id>.jpg                128 px JPEG icons
  previews/<id>.jpg             homebrew's 480x270 previews (themes' and avatars' are the
                                site's own, already on dl.ps3.world beside each download)

  node make-catalog.mjs                  everything
  node make-catalog.mjs --limit 3 --out <dir>      the first 3 of each type (a quick sample)
  node make-catalog.mjs --only theme --out <dir>   one type (homebrew, theme, avatar; comma separated)
                                         (both need --out, so they can't replace the full catalogue)
  node make-catalog.mjs --out <dir>      write somewhere else (the default is CONFIG.outDir)
  node make-catalog.mjs --verify         re-hash every page and file the written index lists

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
  appFolder: 'homebrewtools',					// under homebrewRoot (and the bucket): PS3WorldApp-v<version>.pkg, the newest offered as the app's update
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

const SCHEMA = 3;							// 2: an index (catalog.json) and lists in pages of PAGE_SIZE items
											// 3: items can be folders (avatar packs) with a list of their own
const CATALOG_DIR = 'catalog/v1';			// on dl.ps3.world and under outDir
const PAGE_SIZE = 100;
const TYPES = ['homebrew', 'theme', 'avatar'];
// homebrew is split into the site's categories (the manifest's "group"), in this order
const HOMEBREW_LISTS = ['Games', 'Ports', 'Edits', 'Tools', 'Media', 'Misc'];
const ICON_SIZE = 128;
const HOMEBREW_ICON = 256;					// (homebrew's few are drawn up to 300 px at 1080p)
const ICON_BG = { r: 0x1a, g: 0x1c, b: 0x20 };
const HASH_JOBS = 4;

// ---- arguments ----
const args = process.argv.slice(2);
const argVal = name => { const i = args.indexOf(name); return i >= 0 ? args[i + 1] : undefined; };
const LIMIT = argVal('--limit') ? parseInt(argVal('--limit'), 10) : 0;
const ONLY = (argVal('--only') || 'homebrew,theme,avatar').split(',');
const VERIFY = args.includes('--verify');
if (argVal('--out')) CONFIG.outDir = path.resolve(argVal('--out'));
else if (LIMIT || argVal('--only')) {
  // a partial catalogue in the live folder would go up with the next rclone copy
  console.error('--limit and --only need --out <dir>: they would overwrite the full catalogue in ' + CONFIG.outDir);
  process.exit(1);
}

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

const sha256Of = s => crypto.createHash('sha256').update(s).digest('hex');

// the letter the app files a title under (src/ui.c Letter_Of): the first ASCII
// letter, '#' for a digit or anything non-ASCII first; other punctuation is skipped
function letterOf(title) {
  for (const ch of title) {
    if (/[a-z]/i.test(ch)) return ch.toUpperCase();
    if (/[0-9]/.test(ch) || ch.charCodeAt(0) >= 0x80) return '#';
  }
  return '#';
}
const letterRank = t => { const l = letterOf(t); return l === '#' ? 0 : l.charCodeAt(0) - 64; };

// one list: A-Z (or in the order given), written as pages of PAGE_SIZE and a search.txt; returns its index entry
function writeList(name, list, relDir, order) {
  list.sort(order || ((a, b) => letterRank(a.title) - letterRank(b.title) ||
    a.title.localeCompare(b.title, 'en', { sensitivity: 'base', numeric: true }) || a.id.localeCompare(b.id)));
  const dir = path.join(CONFIG.outDir, relDir);
  fs.mkdirSync(dir, { recursive: true });
  const entry = { name, count: list.length, pages: [], search: null, letters: {} };
  list.forEach((s, i) => { const l = letterOf(s.title); if (!(l in entry.letters)) entry.letters[l] = i; });
  for (let p = 0; p * PAGE_SIZE < list.length; p++) {
    const bad = list.slice(p * PAGE_SIZE, (p + 1) * PAGE_SIZE).find(s => !ID_RE.test(s.id));
    if (bad) throw new Error(`id "${bad.id}" (${bad.title}) isn't one the app accepts`);
    const page = {
      schema: SCHEMA, page: p + 1,
      items: list.slice(p * PAGE_SIZE, (p + 1) * PAGE_SIZE).map(s => ({
        id: s.id, type: s.type, title: s.title, author: s.author, version: s.version,
        description: s.description, group: s.group, size: s.size, sha256: s.sha256,
        url: s.url, icon: s.icon, preview: s.preview, requires: s.requires, list: s.list,
      })),
    };
    const json = JSON.stringify(page);
    fs.writeFileSync(path.join(dir, `${p + 1}.json`), json);
    entry.pages.push({ url: `${relDir}/${p + 1}.json`, sha256: sha256Of(json), size: Buffer.byteLength(json) });
  }
  // one line an item, in list order: what the app searches without loading every page
  const clean = s => (s || '').replace(/[\t\r\n]+/g, ' ');
  const search = list.map(s => `${clean(s.title)}\t${clean(s.author)}\t${clean(s.group)}\n`).join('');
  fs.writeFileSync(path.join(dir, 'search.txt'), search);
  entry.search = { url: `${relDir}/search.txt`, sha256: sha256Of(search), size: Buffer.byteLength(search) };
  return entry;
}

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
// (homebrew's are bigger, in icons/hd: sharp at 1080p, and a new address so apps fetch them again)
async function makeIcon(src, id, size = ICON_SIZE, dir = 'icons') {
  if (!src) return '';
  try { await fsp.access(src); } catch { warn(`icon missing for ${id}: ${src}`); return ''; }
  const rel = `${CATALOG_DIR}/${dir}/${id}.jpg`;
  const out = path.join(CONFIG.outDir, rel);
  try {
    const [a, b] = await Promise.all([fsp.stat(src), fsp.stat(out).catch(() => null)]);
    if (b && b.mtimeMs >= a.mtimeMs) return rel;		// made already
    await fsp.mkdir(path.dirname(out), { recursive: true });
    // the PS3 shows JPEG and PNG only: baseline JPEG, letterboxed into a square
    // (failOn none: a few avatar PNGs have damaged chunks but still decode)
    await sharp(src, { failOn: 'none' }).resize(size, size, { fit: 'contain', background: ICON_BG })
      .flatten({ background: ICON_BG })
      .jpeg({ quality: 80, progressive: false, mozjpeg: false, chromaSubsampling: '4:2:0' })
      .toFile(out);
    return rel;
  } catch (e) {
    warn(`icon failed for ${id}: ${e.message}`);
    return '';
  }
}

// homebrew's preview: its site image, letterboxed into 960x540 (the detail screen's box at 1080p)
async function makePreview(src, id) {
  if (!src) return '';
  try { await fsp.access(src); } catch { return ''; }
  const rel = `${CATALOG_DIR}/previews/hd/${id}.jpg`;
  const out = path.join(CONFIG.outDir, rel);
  try {
    const [a, b] = await Promise.all([fsp.stat(src), fsp.stat(out).catch(() => null)]);
    if (b && b.mtimeMs >= a.mtimeMs) return rel;
    await fsp.mkdir(path.dirname(out), { recursive: true });
    await sharp(src, { failOn: 'none' }).resize(960, 540, { fit: 'contain', background: ICON_BG })
      .flatten({ background: ICON_BG })
      .jpeg({ quality: 82, progressive: false, mozjpeg: false, chromaSubsampling: '4:2:0' })
      .toFile(out);
    return rel;
  } catch (e) {
    warn(`preview failed for ${id}: ${e.message}`);
    return '';
  }
}

// ---- ids ----
// the app (src/catalog.c Id_Ok) skips any other id: it names cache files
const ID_RE = /^[a-z0-9][a-z0-9-]{0,62}$/;
const slug = (base, max) => base.toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-|-$/g, '').slice(0, max).replace(/-+$/, '');
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
      _list: t.category || 'Misc',
      url: rel,
      requires: 'any',
      _file: localFor(rel),
      _icon: img && t.hasImage ? localFor(img) : '',
      _preview: img && t.hasImage ? img : '',
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
      _list: !a.region || a.region === 'Unknown' ? 'Other' : a.region,
      _pack: a.pack,
      url: rel,
      requires: 'hen',
      _file: localFor(rel),
      _icon: img ? localFor(img) : '',
      _preview: img || '',
    });
  }
  return out;
}

// ---- when each item first appeared: "Recently added" is the last RECENT_DAYS ----
// (the first run marks everything there as "before": the list starts with what's added after it)
const RECENT_DAYS = 30;
let seen = {};
const seenOf = i => i._seen || seen[i.id] || 'before';
const newestFirst = (a, b) => seenOf(b).localeCompare(seenOf(a)) || a.title.localeCompare(b.title, 'en', { sensitivity: 'base', numeric: true });
const largestFirst = (a, b) => b.size - a.size || a.title.localeCompare(b.title, 'en', { sensitivity: 'base', numeric: true });

// a section's list, and the same in two more orders the app can switch to (Select): newest
// first, largest first. Each is a list of its own (pages, search.txt) under the list's "sorted";
// apps before 1.2.0 don't look there
function topList(name, rows, relDir) {
  const entry = writeList(name, rows, relDir);
  entry.sorted = [writeList(name, [...rows], `${relDir}/newest`, newestFirst), writeList(name, [...rows], `${relDir}/largest`, largestFirst)];
  return entry;
}

// avatars as folders: an item per pack, whose "list" (the index's list entry, as a string)
// is the pack's avatars in pages of their own under relDir/packs/<pack>
function packFolders(avatars, relDir) {
  const packs = {};
  for (const a of avatars) (packs[a._pack] ||= []).push(a);
  return Object.values(packs).map(av => {
    const entry = writeList(av[0].group, av, `${relDir}/packs/${av[0]._pack}`);
    return {
      id: slug(`pk-${av[0]._pack}`, 63),		// the same in every list the pack is in
      type: 'avatar',
      title: av[0].group,
      author: '',
      version: '',
      description: `${av.length} avatar${av.length === 1 ? '' : 's'}`,
      group: [...new Set(av.map(a => a._list))].join(', '),
      size: av.reduce((n, a) => n + a.size, 0),
      sha256: entry.pages[0].sha256,		// (its first page: a folder has no file of its own)
      url: entry.pages[0].url,
      icon: (av.find(a => a.icon) || {}).icon || '',
      requires: 'hen',
      list: JSON.stringify(entry),
      _seen: av.map(seenOf).sort().pop(),		// (its newest avatar's, for "Recently added")
    };
  });
}

// ---- the app's own update: the newest PS3WorldApp-v<version>.pkg in appFolder, or null ----
async function appUpdate() {
  const dir = path.join(CONFIG.homebrewRoot, CONFIG.appFolder);
  let names;
  try { names = await fsp.readdir(dir); } catch { warn(`no ${dir}: the catalogue offers no app update`); return null; }
  const ver = v => v.split('.').map(Number);
  const newer = (a, b) => { for (let i = 0; i < 3; i++) if ((a[i] || 0) !== (b[i] || 0)) return (a[i] || 0) > (b[i] || 0); return false; };
  let best = null;
  for (const n of names) {
    const m = /^PS3World(?:App)?-v?(\d+\.\d+\.\d+)\.pkg$/i.exec(n);		// PS3WorldApp-v1.0.0.pkg (or PS3World-1.0.0.pkg)
    if (m && (!best || newer(ver(m[1]), ver(best.version)))) best = { version: m[1], file: n };
  }
  if (!best) { warn(`no PS3WorldApp-v<version>.pkg in ${dir}: the catalogue offers no app update`); return null; }
  const info = await fileInfo(path.join(dir, best.file));
  return { version: best.version, url: encodePath(`${CONFIG.appFolder}/${best.file}`), size: info.size, sha256: info.sha256, _file: path.join(dir, best.file) };
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
    s.icon = s.type === 'homebrew' ? await makeIcon(s._icon, s.id, HOMEBREW_ICON, 'icons/hd') : await makeIcon(s._icon, s.id);
    s.preview = s.type === 'homebrew' ? await makePreview(s._icon, s.id) : s._preview || '';
    items.push(s);
  });
  const app = ONLY.includes('homebrew') ? await appUpdate() : null;
  fs.writeFileSync(cachePath, JSON.stringify(hashCache));

  const seenPath = path.join(CONFIG.outDir, '.cache', 'first-seen.json');
  let first = !fs.existsSync(seenPath);
  try { seen = first ? {} : JSON.parse(fs.readFileSync(seenPath, 'utf8')); } catch { seen = {}; first = true; }
  const today = new Date().toISOString().slice(0, 10);
  for (const i of items) if (!seen[i.id]) seen[i.id] = first ? 'before' : today;
  fs.writeFileSync(seenPath, JSON.stringify(seen));
  const cutoff = new Date(Date.now() - RECENT_DAYS * 864e5).toISOString().slice(0, 10);
  const isRecent = i => seen[i.id] !== 'before' && seen[i.id] >= cutoff;

  // the index: each section's lists (homebrew: one per category; themes, avatars: all, then one
  // per category or region, the biggest first, Other last), each in pages
  const index = {
    schema: SCHEMA,
    generated: new Date().toISOString().replace(/\.\d+Z$/, 'Z'),
    base_url: CONFIG.baseUrl,
    page_size: PAGE_SIZE,
    sections: [],
  };
  // (apps before 0.6.0 ignore it)
  if (app) index.app = { version: app.version, url: app.url, size: app.size, sha256: app.sha256 };
  for (const type of TYPES) {
    const all = items.filter(i => i.type === type);
    const section = { type, count: all.length, lists: [] };
    if (type === 'homebrew') {
      const names = [...HOMEBREW_LISTS, ...new Set(all.map(i => i.group).filter(g => !HOMEBREW_LISTS.includes(g)))];
      for (const name of names) {
        const list = all.filter(i => i.group === name);
        if (list.length) section.lists.push(topList(name, list, `${CATALOG_DIR}/homebrew/${name.toLowerCase().replace(/[^a-z0-9]+/g, '-')}`));
      }
      const recent = all.filter(isRecent);
      if (recent.length) section.lists.push(writeList('Recently added', recent, `${CATALOG_DIR}/homebrew/recent`, newestFirst));
    } else {
      // (avatars: a list's rows are its packs)
      const rows = (list, relDir) => (type === 'avatar' ? packFolders(list, relDir) : list);
      section.lists.push(topList(`All ${type}s`, rows(all, `${CATALOG_DIR}/${type}/all`), `${CATALOG_DIR}/${type}/all`));
      const recent = all.filter(isRecent);
      if (recent.length)
        section.lists.push(writeList('Recently added', rows(recent, `${CATALOG_DIR}/${type}/recent`), `${CATALOG_DIR}/${type}/recent`, newestFirst));
      const by = {};
      for (const i of all) (by[i._list] ||= []).push(i);
      // the biggest first (avatars: by packs, the count the app shows); "Other" (no region) last
      const size = list => (type === 'avatar' ? new Set(list.map(i => i._pack)).size : list.length);
      for (const [name, list] of Object.entries(by).sort((a, b) =>
        (a[0] === 'Other') - (b[0] === 'Other') || size(b[1]) - size(a[1]))) {
        const relDir = `${CATALOG_DIR}/${type}/${name.toLowerCase().replace(/[^a-z0-9]+/g, '-')}`;
        section.lists.push(topList(name, rows(list, relDir), relDir));
      }
    }
    index.sections.push(section);
  }
  // homebrew's descriptions in other languages: generator/descriptions/<code>.json (id: text),
  // published as lang/<code>.json with what's in the catalogue; the app loads its language's
  const descDir = path.join(HERE, 'descriptions');
  const hb = items.filter(i => i.type === 'homebrew' && i.description);
  for (const f of fs.existsSync(descDir) ? fs.readdirSync(descDir).filter(f => /^[a-z]{2}\.json$/.test(f)).sort() : []) {
    const code = f.slice(0, 2), all = readJson(path.join(descDir, f)), out = {};
    for (const i of [...hb].sort((a, b) => a.id.localeCompare(b.id))) if (all[i.id]) out[i.id] = all[i.id];
    const missing = hb.filter(i => !all[i.id]).map(i => i.title);
    if (missing.length) warn(`descriptions/${f}: none yet for ${missing.join(', ')} (English shows)`);
    const text = JSON.stringify(out), rel = `${CATALOG_DIR}/lang/${code}.json`;
    fs.mkdirSync(path.join(CONFIG.outDir, CATALOG_DIR, 'lang'), { recursive: true });
    fs.writeFileSync(path.join(CONFIG.outDir, rel), text);
    (index.descriptions ||= {})[code] = { url: rel, sha256: sha256Of(text), size: Buffer.byteLength(text) };
  }

  const json = JSON.stringify(index);
  const outFile = path.join(CONFIG.outDir, CATALOG_DIR, 'catalog.json');
  fs.writeFileSync(outFile, json);

  const lists = index.sections.flatMap(s => s.lists);
  const pages = lists.flatMap(l => l.pages);
  const count = t => items.filter(i => i.type === t).length;
  console.log(`\nwrote ${outFile} (the index: ${(json.length / 1024).toFixed(1)} KB)`);
  console.log(`  ${items.length} items: ${count('homebrew')} homebrew, ${count('theme')} themes, ${count('avatar')} avatars`);
  for (const s of index.sections)
    for (const l of s.lists)
      console.log(`  ${s.type}${l.name ? '/' + l.name : ''}: ${l.count} items, ${l.pages.length} page(s), search.txt ${(l.search.size / 1024).toFixed(0)} KB`);
  console.log(`  pages: ${pages.length}, the biggest ${(Math.max(...pages.map(p => p.size)) / 1024).toFixed(1)} KB`);
  console.log(`  ${items.filter(i => !i.icon).length} without an icon`);
  if (app) console.log(`  app update offered: PS3 World ${app.version} (${app.url}, ${(app.size / 1048576).toFixed(1)} MB)`);
  report();
  console.log('\nUpload (you run this; copy never deletes anything on R2):');
  if (app)		// the PKG first: the index mustn't offer it before it's there
    console.log(`  rclone copy "${path.join(CONFIG.homebrewRoot, CONFIG.appFolder).replace(/\//g, '\\')}" ${CONFIG.bucket}/${CONFIG.appFolder} --progress`);
  console.log(`  rclone copy "${path.join(CONFIG.outDir, 'catalog').replace(/\//g, '\\')}" ${CONFIG.bucket}/catalog --progress`);
}

// ---- verify: every file in catalog.json against its sha256, read fresh ----
async function verify() {
  const file = path.join(CONFIG.outDir, CATALOG_DIR, 'catalog.json');
  const index = readJson(file);
  const homebrewByUrl = Object.fromEntries(homebrewSources().map(s => [s.url, s._file]));
  let ok = 0, bad = 0;
  // every page against the index's sha256, then every item's file against the page's
  const cat = { items: [] };
  const lists = index.sections.flatMap(s => s.lists);
  for (let k = 0; k < lists.length; k++)		// (a folder's own list joins the queue)
    for (const p of lists[k].pages) {
      const text = fs.readFileSync(path.join(CONFIG.outDir, p.url), 'utf8');
      if (sha256Of(text) !== p.sha256) { bad++; warn(`MISMATCH page ${p.url}`); continue; }
      for (const it of JSON.parse(text).items)
        if (it.list) lists.push(JSON.parse(it.list));
        else cat.items.push(it);
    }
  for (const f of Object.values(index.descriptions || {}))		// (the translated descriptions)
    if (sha256Of(fs.readFileSync(path.join(CONFIG.outDir, f.url), 'utf8')) !== f.sha256) { bad++; warn(`MISMATCH ${f.url}`); }
  // the other orders: the same items, so only their pages and search files are checked
  for (const l of index.sections.flatMap(s => s.lists).flatMap(l => l.sorted || []))
    for (const f of [...l.pages, l.search])
      if (sha256Of(fs.readFileSync(path.join(CONFIG.outDir, f.url), 'utf8')) !== f.sha256) { bad++; warn(`MISMATCH ${f.url}`); }
  if (index.app) {		// the app's update, checked like an item
    homebrewByUrl[index.app.url] = path.join(CONFIG.homebrewRoot, ...index.app.url.split('/').map(decodeURIComponent));
    cat.items.push({ id: 'ps3world-app', type: 'homebrew', ...index.app });
  }
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
