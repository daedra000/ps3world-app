# ps3world-app

**PS3 World** for the PlayStation 3 (HEN and CFW): browse the [PS3 World](https://ps3.world) archives on the console and download homebrew, themes and avatars straight from `dl.ps3.world`, without a PC.

## Screenshots

| | |
|---|---|
| ![The home screen: Homebrew, Themes and Avatars as cards](docs/screenshots/home.png) | ![Homebrew > Ports: icon, title, author, size per row](docs/screenshots/homebrew-ports.png) |
| Home: the three sections, styled as ps3.world's home page | A list: Homebrew > Ports |
| ![A theme's detail screen with its preview](docs/screenshots/theme-detail.png) | ![An avatar pack opened as a list](docs/screenshots/avatar-pack.png) |
| A theme: its preview, and where it saves (`/dev_hdd0/theme`) | Avatars: a pack, opened as a folder |

## Features

- **Homebrew, themes and avatars** in the site's categories: Homebrew by Games, Ports, Tools and Media; themes by their 17 categories; avatars by region, in packs.
- **Search** any list (Triangle), and jump by letter with L2/R2.
- **Previews** of what you're downloading: a theme's screenshot, an avatar, a homebrew's art.
- **Saves where the PS3 looks:** PKGs to `/dev_hdd0/packages` (Package Manager > Install Package Files), themes to `/dev_hdd0/theme`, avatars to the chosen user's `psn_avatar` folder.
- **Checked downloads:** every file's size and sha256 are checked before it's kept, and a stopped download resumes where it left off.
- **A download queue:** start several and keep browsing; the one running shows bottom right.
- **My downloads** (Triangle on the home screen): the queue, then everything on the HDD with its date. Delete a file (Square twice), or get a homebrew's new version.
- **Free space** on the HDD, on the home screen.
- **Updates:** when a newer PS3 World is out, the home screen says so, and Square downloads it.
- **Works offline** with the last catalogue it loaded.
- **12 languages:** English, Spanish, Portuguese, French, German, Italian, Dutch, Polish, Turkish, Danish, Indonesian and Arabic (joined letters, right to left). It follows the PS3's language; Select on the home screen picks another. The words are in `data/lang/<code>.txt`, one "English<TAB>translation" a line: corrections welcome.

## Controls

| Button | Does |
|---|---|
| D-pad | move (Left/Right: a page at a time in lists) |
| Cross | open, download |
| Circle | back |
| Square | clear a search; cancel a download; delete in My downloads; get an update on the home screen |
| Triangle | search a list; My downloads on the home screen |
| Select | the language, on the home screen |
| L1 / R1 | in a list, the previous / next category (Games, Ports, Edits...); on the categories screen, the previous / next section |
| L2 / R2 | the previous / next letter |

Hold R1 while the app starts to try the graphics chip again after a freeze, or L1 to use the CPU renderer that once.

## Adding content and releases

The app reads a catalogue that `generator/make-catalog.mjs` builds from the sites' data and `generator/homebrew.manifest.json` ([generator/README.md](generator/README.md)). After adding to a site or the manifest, rebuild it and upload it; consoles pick it up the next time the app starts.

To release a new version, put its PKG on R2 as `homebrewtools/PS3WorldApp-v<version>.pkg` and rebuild the catalogue: apps from 0.6.0 on offer it.

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

Title ID `PSWD00001`; it installs to `/dev_hdd0/game/PSWD00001/`. The log is `USRDIR/ps3world_log.txt`, and `USRDIR/downloaded.txt` lists what's been downloaded. `docs/TEST-CHECKLIST.md` is the on-console test.

## Licence

GPL-2.0. Based on [Resonant for Plex](https://github.com/daedra000/Resonant-PS3) (HTTPS client, drawing, fonts, input, worker threads, build). Third-party licences are in `data/licenses/`.
