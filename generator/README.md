# Catalogue generator

Builds `catalog/v1/catalog.json` and `catalog/v1/icons/*.jpg` for the PS3 World app from the site build data and the local copies of every download. It never uploads anything.

```
npm install
node make-catalog.mjs              # everything (first run hashes ~60 GB; later runs use the cache)
node make-catalog.mjs --limit 3    # a quick sample: the first 3 of each type
node make-catalog.mjs --verify     # re-hash every file listed in catalog.json, ignoring the cache
```

Paths are in `CONFIG` at the top of `make-catalog.mjs`. Output goes to `G:\ps3world-catalog`. The hash cache is kept in `.cache\` there, and it is not part of the upload.

## Sources

| Type | Metadata | Files |
|---|---|---|
| Themes | `G:\ps3-themes\_site\data\themes.json` (NSFW skipped) | `Best-PS3-Themes\PS3_Themes`, `PS3_Themes` |
| Avatars | `G:\PS3 Avatars\_site_avatars\data\avatars.json` | `D:\PS3 Avatars` |
| Homebrew | `homebrew.manifest.json` (this folder) | `D:\PS3 WORLD R2` |

## homebrew.manifest.json

You maintain this file. Each entry looks like this:

```json
{
  "file": "homebrewtools/pkgi-ps3.pkg",
  "title": "PKGi PS3",
  "author": "",
  "version": "",
  "description": "One or two sentences.",
  "group": "Tools",
  "requires": "hen",
  "icon": "images/homebrew/pkgi-ps3.webp"
}
```

- `file`: the path under `D:\PS3 WORLD R2`, which is also the path on dl.ps3.world.
- `requires`: `any`, `hen` (HEN or CFW) or `cfw` (CFW only).
- `icon`: relative to the main site folder (`siteRoot` in CONFIG). Any image type works, and it's converted to a 128 px JPEG. Leave it empty for no icon.

## Upload

The generator prints the command to run when it finishes:

```
rclone copy "G:\ps3world-catalog\catalog" R2:ps3wrld-content/catalog --progress
```
