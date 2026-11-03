# PS3 World: test checklist (real PS3, HEN)

Tick each box. Where a step fails, note what the screen said and copy `/dev_hdd0/game/PSWD00001/USRDIR/ps3world_log.txt` off the console (FTP or multiMAN). The log records every request, download, checksum and path.

## Before you start

- [ ] The catalogue is uploaded: `rclone copy "G:\ps3world-catalog\catalog" R2:ps3wrld-content/catalog --progress`, then <https://dl.ps3.world/catalog/v1/catalog.json> opens in a browser.
- [ ] The PS3 is on HEN (enabled this boot), online, and the date and time are right (Settings > Date and Time Settings). TLS checks certificate dates against the clock.
- [ ] `PS3World.pkg` from the latest GitHub Actions run (artifact **PS3World**) is installed: copy it to `/dev_hdd0/packages/` or a USB drive, then Package Manager > Install Package Files.

## 1. Open the app

- [ ] **PS3 World** is on the XMB under Game with the PS3 World icon, and the waves picture shows behind it when highlighted.
- [ ] It opens to "Loading the catalogue..." (a few seconds at most: only the small index downloads), then the home screen: three cards as on ps3.world's home page (Homebrew large on the left, Themes and Avatars on the right), each with its character art, a cyan kicker, its title, a line of text and category chips.
- [ ] D-pad moves between the cards: the selected one has a blue outline and brighter art.
- [ ] Cross on **Homebrew** shows its categories as four art cards: Games 5, Ports 17, Tools 13, Media 5. Circle goes back home.
- [ ] Cross on **Themes** shows a grid: All themes 13,303, then 17 categories (Games, Anime & Comics, Movies & TV...). Down scrolls the grid. **Avatars**: All avatars 507 packs, then US, EU, Japan, Asia, and Other last.
- [ ] The Homebrew card's title doesn't cover Kratos's face.
- [ ] R1 from any category screen or list goes to the next section's categories (R1 crashed in the first build).
- [ ] Avatars > Asia: the BlazBlue and Dynasty Warriors packs show their icons (their ids were too long before 0.5.3, and the icons never loaded).
- [ ] Open the app and quit straight away (PS button > Quit Game). Reopen it: the log says "drawn by the RSX", not "the CPU draws". If the CPU ever draws after a freeze, hold R1 while the app starts to try the graphics chip again.

- [ ] Send me the two `selftest:` lines from `ps3world_log.txt` (whether ".." goes up a folder, and whether rename replaces a file), and any `https: recv failed, errno` line.

## 2. Browse

- [ ] Cross opens a category's list, Circle goes back to the categories, and Circle again goes home.
- [ ] Up/Down move one row, Left/Right a screen. Holding a direction repeats and speeds up.
- [ ] In Themes > All themes, hold Down past item 100: rows show "Loading..." for a moment while the next page arrives, then fill in. Scrolling back up is instant.
- [ ] Icons appear as you scroll (first time from the server, then from the HDD). Theme rows show a small 16:9 preview of the theme, not a square icon.
- [ ] A theme's detail screen shows its preview large (640x360) on the left, with Author, Size, Category, Works on and Saves to on the right, then the description; under the preview, how to use it after it downloads. An avatar shows square in the middle of the preview box; a homebrew item shows its screenshot. "Loading the preview..." shows briefly the first time.
- [ ] L1/R1 switch between Homebrew, Themes and Avatars.
- [ ] In All themes, R2 jumps to the next letter and L2 to the previous one. The letter and position show bottom right.
- [ ] Triangle opens the keyboard. Search "wolf" in All themes: only matching themes are listed, with "Search: "wolf"" at the top. Square (or Circle) clears it.
- [ ] Cross opens an item's detail screen (description, size, "Needs HEN or CFW" / "Any PS3", and "Saves to ..."). Circle goes back to the same row.

## 3. Homebrew download (PKG)

- [ ] Homebrew > Games: open **Avoidance** (285 KB). It says "Saves to /dev_hdd0/packages/Avoidance v1.3.pkg".
- [ ] Cross: the progress bar, %, MB and speed move, then "Checksum OK (278 KB). Saved to /dev_hdd0/packages/Avoidance v1.3.pkg".
- [ ] Back in the list, Avoidance has a green **Downloaded** badge.
- [ ] XMB > Package Manager > Install Package Files lists **Avoidance v1.3.pkg**, and it installs. [unverified until this step passes]

## 4. Cancel and resume

- [ ] Open a big homebrew item (Homebrew > Ports > **Undertale**, 148 MB) and press Cross, then Circle part way: "Download cancelled", and no `Undertale.pkg.part` is left in `/dev_hdd0/packages/`.
- [ ] Start it again, and part way pull the network cable (or turn Wi-Fi off). After about 30 s it shows the network error and "Press X to resume where it stopped".
- [ ] Reconnect and press Cross: "Resumed from N MB" shows, and it finishes with "Checksum OK".
- [ ] Optional: start it again and quit to the XMB (PS button > Quit Game) part way. Reopen the app and download again: it resumes rather than starting over.

## 5. Theme download (.p3t)

- [ ] Open any theme: it says "Saves to /dev_hdd0/theme/<name>.p3t" (no USB drive needed).
- [ ] Cross: "Checksum OK ... Saved to /dev_hdd0/theme/<name>.p3t".
- [ ] XMB > Settings > Theme Settings > Theme: the theme is listed and applies. (FTPing a .p3t there works, so this should too.)

## 6. Avatar download (.edat)

- [ ] Avatars > All avatars lists packs ("AeternoBlade  4 avatars · US"). Cross opens a pack as a list of its avatars; Circle goes back to the packs (on the same row).
- [ ] Open any avatar in a pack. With one user on the PS3 it says "Saves to /dev_hdd0/home/0000000N/psn_avatar/PSNA_....edat". With several, Cross first asks which user.
- [ ] Cross: "Checksum OK ... Saved to ...psn_avatar/PSNA_....edat".
- [ ] XMB > PlayStation Network > Account Management > Avatar (or the profile's avatar setting) shows the new avatar. [unverified until this step passes]

## 7. Offline and errors

- [ ] Quit the app, disconnect the network, then reopen: the lists open from the HDD copy, with "Offline: showing the catalogue from <date>" in yellow. Pages you've already scrolled through show; ones you haven't show the network error in the row.
- [ ] Still offline, open an item and press Cross: "No network connection. Check Settings > Network Settings ...".
- [ ] Set the PS3's date to a year far in the future, reconnect and download: "The secure connection to dl.ps3.world failed. Check the PS3's date and time ...". Set the date back afterwards.
- [ ] Disk full: hard to set up on purpose. The CI test covers it (an impossible size is refused with "Not enough free space (X MB needed, Y MB free)").
- [ ] Checksum mismatch and 404 are covered by the CI test (`download: checksum mismatch deleted`, `download: 404`).

## 8. Files are where the app said

- [ ] With multiMAN or FTP, check that each file from steps 3, 5 and 6 exists at the exact path the app showed, with the size shown.
- [ ] `/dev_hdd0/game/PSWD00001/USRDIR/downloaded.txt` lists those three, with their sha256 hashes and paths.
