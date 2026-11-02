# ps3world-app

**PS3 World** for the PlayStation 3 (HEN and CFW): browse the [PS3 World](https://ps3.world) archives on the console and download homebrew, themes and avatars straight from `dl.ps3.world`, without a PC.

The app reads one index, `https://dl.ps3.world/catalog/v1/catalog.json`, which is made by the generator in [`generator/`](generator/README.md).

## Layout

| Path | What |
|---|---|
| `src/` | the app (C, PSL1GHT); `src/ps3/` is the console backend |
| `src/catalog.c` | catalog.json, parsed and checked |
| `generator/` | Node script that builds catalog.json and the icons from the site build |
| `tools/https_test.c` | the app's HTTPS client run on Linux against dl.ps3.world (CI) |
| `tools/make-art.mjs` | ICON0, PIC1 and the in-app background, from the site |
| `.github/workflows/ps3.yml` | CI: HTTPS test, PS3 toolchain (cached), EBOOT + PKG |

## Building

Push to GitHub and Actions builds `PS3World.pkg` (artifact **PS3World**). The toolchain (ps3toolchain + PSL1GHT, pinned) is built once and cached; Mbed TLS 3.6.7 is built per run by `tools/build-mbedtls.sh`.

Title ID `PSWD00001`; it installs to `/dev_hdd0/game/PSWD00001/`. The log is `USRDIR/ps3world_log.txt`.

## Licence

GPL-2.0. Based on [Resonant for Plex](https://github.com/daedra000/Resonant-PS3) (HTTPS client, drawing, fonts, input, worker threads, build). Third-party licences are in `data/licenses/`.
