# PS3 World: test checklist (real PS3, HEN)

Tick each box. Where a step fails, note what the screen said and copy `/dev_hdd0/game/PSWD00001/USRDIR/ps3world_log.txt` off the console (FTP or multiMAN). The log records every request, download, checksum and path.

## Before you start

- [ ] The catalogue is uploaded: `rclone copy "G:\ps3world-catalog\catalog" R2:ps3wrld-content/catalog --progress`, then <https://dl.ps3.world/catalog/v1/catalog.json> opens in a browser.
- [ ] The PS3 is on HEN (enabled this boot), online, and the date and time are right (Settings > Date and Time Settings). TLS checks certificate dates against the clock.
- [ ] A FAT32 USB drive is ready for the theme test (leave it out until step 5).
- [ ] `PS3World.pkg` from the latest GitHub Actions run (artifact **PS3World**) is installed: copy it to `/dev_hdd0/packages/` or a USB drive, then Package Manager > Install Package Files.

## 1. Open the app

- [ ] **PS3 World** is on the XMB under Game with the PS3 World icon, and the waves picture shows behind it when highlighted.
- [ ] It opens to "Loading the catalogue..." (a few seconds at most: only the small index downloads), then **Homebrew**'s categories: Games 5, Ports 19, Tools 13, Media 5.
- [ ] The section tabs show counts that match the catalogue: Homebrew 42, Themes 13,303, Avatars 6,323.
- [ ] R1 from Homebrew to Themes and Avatars opens their lists straight away (this crashed in the first build).

## 2. Browse

- [ ] In Homebrew, Cross opens a category and Circle goes back to the categories.
- [ ] Up/Down move one row, Left/Right a screen. Holding a direction repeats and speeds up.
- [ ] In Themes, hold Down past item 100: rows show "Loading..." for a moment while the next page arrives, then fill in. Scrolling back up is instant.
- [ ] Icons appear as you scroll (first time from the server, then from the HDD).
- [ ] L1/R1 switch between Homebrew, Themes and Avatars.
- [ ] In Themes, R2 jumps to the next letter and L2 to the previous one. The letter and position show bottom right.
- [ ] Triangle opens the keyboard. Search "wolf" in Themes: only matching themes are listed, with "Search: "wolf"" at the top. Square (or Circle) clears it.
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

- [ ] With no USB drive in, open any theme: the yellow line says to plug in a FAT32 USB drive, and Cross shows the same message.
- [ ] Plug the drive in, then reopen the theme: "Saves to /dev_usb000/PS3/THEME/<name>.p3t" (the number can differ).
- [ ] Cross: "Checksum OK ... Saved to /dev_usb00N/PS3/THEME/<name>.p3t".
- [ ] XMB > Settings > Theme Settings > Theme > Install: the theme is listed and installs. [unverified until this step passes]
- [ ] Optional (decides whether v2 can save themes to the HDD): FTP the same .p3t into `/dev_hdd0/theme/` and check whether it shows in Theme Settings > Theme without installing.

## 6. Avatar download (.edat)

- [ ] Open any avatar. With one user on the PS3 it says "Saves to /dev_hdd0/home/0000000N/psn_avatar/PSNA_....edat". With several, Cross first asks which user.
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
