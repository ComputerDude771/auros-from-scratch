# AurOS end-user journey audit: great-grandma, any Windows PC, under 10 minutes

Repository `/home/user/auros-from-scratch`, branch `claude/brave-hypatia-x72g1m` at `424ed96`. Read-only audit, 2026-10-23.

**The short answer is that she does not succeed, on any PC, in 10 minutes or in 10 hours.** The code has at least three separate places where she stops for good, even on a perfect 2017 SSD laptop:

1. The website tells her not to download it. The only download sits behind the checkbox "This PC's files don't matter to me".
2. Very likely (from reading the code; needs a hardware check): on a laptop that is on Wi-Fi with no cable, the install shrinks Windows and then gives up, because the Wi-Fi check after the restart cannot load Wi-Fi firmware.
3. In the default desktop ("rail"), "Get more programs" is not on the Home screen and nothing else in that desktop opens it.

Even on the happy path (cable plugged in, nothing refused), a 2017 SSD laptop needs about **35 to 50 minutes**. A 2015 laptop with a 1 TB hard disk needs **about 4 to 5 hours**. Two things in the code cause the hard-disk time: it reads every free sector of the disk, and it shrinks Windows to its absolute minimum.

How I checked:
- I rendered the website with Playwright at 1366x768 and 390x844.
- I built the *current* wizard (`wizard.c` plus the shared sources, with a stub baked header) using mingw. I rendered every page under Wine/Xvfb through its own `--shot` harness. The committed `docs/shots/wizard/*` images are stale: they still show a USB stick, "Your choice" and "Make it yours".
- I read `preflight.c`, `fix_win.c`, `phases.c`, `plat_win.c`, `src/aurstage/*`, `src/aurscreen/screen.c`, `src/aurshell/{apps.c,welcome.c,net.c,foot.c,main.c,layouts/rail.c}`, `build/forge`, `build/staging`, `profiles/desktop.profile` and `rootfs/usr/lib/auros/*`.

Screenshots are in this folder:
- website: `index-*.png`, `guide-desk-full.png`
- current wizard: `wizard-*.png`

Every "% of PCs" figure is my estimate for consumer Windows PCs from 2015 to 2019. None of them is measured. Each finding I inferred from code but could not run on hardware is marked **(inferred)**.

---

## 1. The journey, step by step

Columns:
- **Attn**: seconds of her reading, deciding, clicking or typing.
- **Wait**: seconds she waits.
- Times are for a 2017 laptop with a SATA SSD on 50 Mbit/s, unless a step says otherwise.

### A. The website (`website/index.html`, `website/assets/drive.js`)

| # | What she sees (quoted) | What she must do | Attn | Wait | Severity |
|---|---|---|---|---|---|
| A1 | Top line: "LBA 0 · protective MBR · You are reading a hard drive. Scroll down it." (`index.html:27`). Small gold monospace: "AurOS is a free operating system that gives an old Windows PC a second life." (`:33`). Huge headline "I'm − 11 + years old. I still work." with +/- buttons (`:34`). A countdown about Microsoft updates (`:35`). | Work out what this is. Nothing on the first screen says "download", "install" or "how". The left margin shows a "read/write head" with LBA numbers. | 30-60 | 1 | HIGH |
| A2 | "this bar is my real hard drive · 256 GB · drawn to scale" (`:41`). The two biggest buttons on the page: "▶ Watch me become AurOS" and "Scroll through my disk" (`:48-49`). Stickers: "Secure Boot: on", "18 power cuts survived". | Neither button downloads anything. "Watch me" plays a fake restart that she can mistake for the real thing. | 30-90 | 0 | HIGH |
| A3 | A table of partitions: "GPT · LBA 1 · The table every computer reads first. It is also this page's menu." FIRST LBA/LAST LBA columns at 12 px (`:62-68`). | Nothing usable for her. | 10-30 | 0 | MEDIUM |
| A4 | A floating round power button in the bottom-right corner: "My power button. Go on." (`:274-277`). Pressing it blacks out the page with "Ow. No power." (`:298-300`). | Someone who is afraid of breaking things will either avoid it or be frightened by it. | 5-20 | 0 | MEDIUM |
| A5 | Sections "How I start, with Secure Boot left on" (shim, GRUB, kernel, `\EFI\AurOS\shimx64.efi`), "Everything you have on me now", "Where AurOS would go" (with **"Time: An evening."**, `:145`), "The way back", "My maker's recovery tools", a SMART table of test counts, then "Bad sectors I know about", including "**Not yet tried on a real PC's drive.**" (`:232`) and "**Windows will warn you about the download.**" (`:233`). | Scroll about 11 screens on a desktop (about 14 on a phone). There are **1,730 words before the download button**: "LBA" ×12, "EFI" ×11, "partition" ×10, "firmware" ×7, "kernel" ×7 (measured). Body text is fine (20 px), but `.sha`, `.lba0`, `.drive-top` and the GPT table are 10.5-12 px. | 120-480 | 0 | **BLOCKER** (she does not find the download, or reads "not yet tried on a real PC" and "an evening" and stops) |
| A6 | The very bottom, "LBA 500,118,191 · backup GPT header": "Try me on a spare PC." "If you have a PC whose files you don't need, the test build is here." Three checkboxes (`:260-262`): "This PC's files don't matter to me." / "I've read what Windows and antivirus will say about an unsigned file." / "If a screen turns red, I'll take a photo of its last line." | The download button stays disabled until all three are ticked (`drive.js:685-693`). For her the first box is false and the second asks her to have read something she hasn't. | 30-90 | 0 | **BLOCKER** (by design for a pre-release, but for this goal she has been told not to proceed) |
| A7 | "Download the test installer", and "Unsigned test build, v4. Before running it, check its SHA-256 is ef903efd…" (`:264-265`). | Click. Checking a SHA-256 is meaningless to her. | 5 | 0 | MEDIUM |
| A8 | If she opened the link on a **phone** (from an email or Facebook): nothing says "open this on the Windows computer you want to change". | She can't use an .exe on a phone. | n/a | n/a | MEDIUM |

### B. Getting the file to run (Windows, browser, antivirus)

| # | What she sees | What she must do | Attn | Wait | Severity |
|---|---|---|---|---|---|
| B1 | The browser downloads about 35 MB from `github.com/.../raw/image-desktop/AurOS-Installer-test.exe`. Edge and Chrome both show *"AurOS-Installer-test.exe isn't commonly downloaded"* for a new unsigned .exe. In Edge the file stays blocked until she goes "…" → Keep → Show more → Keep anyway. | Three to five clicks, partly hidden behind menus, every one of which says "don't". | 30-120 | 6-10 | **BLOCKER** (signing; `docs/SIGNING.md`) |
| B2 | Avast or AVG (and often an OEM McAfee/Norton trial): "Suspicious file detected", held "for a few hours" (`docs/TRY-IT.md`). | Add an exception in the antivirus settings. She cannot. | n/a | hours | **BLOCKER** for PCs with such antivirus (estimated 25-40% of this group) |
| B3 | Opening it: "Windows protected your PC". | More info → Run anyway. On Windows 11 with Smart App Control there is no Run anyway at all. | 20-40 | 0 | **BLOCKER** (signing) |
| B4 | UAC: "Do you want to allow this app from an unknown publisher to make changes to your device?" | Yes. (On a standard account she would need an admin password.) | 5-10 | 2 | MEDIUM |

### C. The Windows wizard (`src/aurbridge/wizard.c`). Screens rendered from current code: `wizard-*.png`

Window size is 1120×760 at 96 dpi. Fonts: body 15 px, small 13 px, tiny 11 px (`wizard.c:3396-3405`). Muted text is `#55606E` on `#161C28`, a contrast of about **2.7:1**. That fails WCAG AA and breaks EASY rule 3. It is used for the "test version" line, the Ready footnote and every footer hint.

The path is Welcome → Check → (Stop) → Before we start → What will happen → Ready → Installing (`RAIL[]`, `wizard.c:249-256`; `nav_allowed` `:1134-1165`). The Choose, Desktop and Personalize pages are disabled. The wizard has **about 1,100 words** across its pages.

| # | What she sees (quoted) | What she must do | Attn | Wait | Severity |
|---|---|---|---|---|---|
| C1 | **Welcome** (`:1787-1820`): "Let's put AurOS on this PC". "…without you having to understand any of it." "Windows stays exactly where it is … **Each time you switch the PC on, you pick which one you want.**" (`:1801`; not true: after "Yes", AurOS starts by default), "You can change your mind", "We look at this PC first, and we will say no". | Press **Get started**. | 30-45 | 0 | LOW |
| C2 | **Checking this PC** (`:1824-1891`): "We only read. Nothing on this PC is written to or changed." Ten rows reveal at 120 ms each (`:3483`). Result banner: "All clear. This PC can take AurOS." Or, with a warning: "**There is 1 thing worth knowing about, coming up next.**" (`:1846`). Nothing comes next: warnings only appear as one coloured line in the list, e.g. "This PC uses Intel Rapid Storage Technology". | Press **Continue**. | 10-20 | 5-15 | LOW (MEDIUM with a warning: jargon, and a promise the wizard doesn't keep) |
| C3 | **Stop page**, if anything blocks (`:2054-2135`): "Almost there. A few things first" or "AurOS can't go on this PC yet". One line per problem (`fix_win.c:14-134`). Button: "Fix these for me" / "Check again" (`:2969`). See section 2. | Press Fix, or wait, or give up. | 15-60 | 0 to hours | **BLOCKER** for a large share of PCs (section 2) |
| C4 | **Before we start** (`:2140-2184`): "Two things before we go on". Checkbox 1: "**I have a copy of anything I would hate to lose, somewhere other than this PC.**" Checkbox 2: "**I understand there is no rescue USB stick.** … If this PC ever refuses to start at all, getting Windows back will need another computer." Then a OneDrive paragraph ("right-click the OneDrive folder and choose 'Always keep on this device'") and "This is a test version of AurOS…" in muted grey. | Tick 2 boxes, then Continue. An honest great-grandma has no backup, so box 1 makes her stop or lie. Box 2 frightens her. | 45-90 | 0 | **HIGH** (many stop here) |
| C5 | **Exactly what is about to happen** (`:2226-2296`): "Read this properly. It is the whole truth…". About 330 words: Fast Startup and hibernation, "about 5 GB" downloaded, the drive "made smaller", "start-up menu", "There is no rescue USB stick in this version. If this PC will not start at all, you will need another computer to repair it." (in red). Then "**Type AGREE below to say you have read this.** We ask you to type it rather than tick a box because a box is too easy to tick without reading." (`:2278`). | Read, scroll, type `AGREE` (5 characters; focus is put in the box for her, `:3201`; capitals not required). Press "I agree — continue". | 90-150 | 0 | HIGH (longest page, typing, a scary red line) |
| C6 | **Ready when you are** (`:2746-2848`): a 9-row summary. Some rows are wrong. "Room for AurOS: **About 28 GB**, taken from the N GB of empty space on drive C:" (`:2779`) is false: staging takes *all* free space (section 1-E). "How long: **About 40 minutes**" (`:2814`) is false on a hard disk (hours). Checkbox: "**Yes: SAMSUNG MZ7LN256HAJQ-000H1 (238 GB) is the drive I want to change.**" (the model string, `:2834`). | Tick, then **Start installing**. The drive model means nothing to her. | 45-75 | 0 | MEDIUM |
| C7 | The checks run again (`start_check(PAGE_PROGRESS)`, `:3206`), then **Setting up AurOS** (`:2854-2940`). A warning banner; 9 phases (`PHASES[]`, `:1191-1220`). "Get AurOS — **About 5 GB** is downloaded" (`:1208`; the download is really about 1.8 GB). "Try it out … **Wi-Fi, screen brightness, sound and sleep are tested**" (`:1216`; sleep is not tested). "Bring your files across … **Only now does anything become permanent**" (`:1220`; the shrink after the restart is permanent). Progress is a 96-px bar plus 11-px log lines ("downloading AurOS, part 3 of 20"). There is no time remaining. | Wait. **The download only starts now**, after every page. Nothing keeps Windows awake (`SetThreadExecutionState` isn't used anywhere), so a long download can be cut off when the PC goes to sleep. | 0 | see below | HIGH (the wait comes after all the reading, not during it) |
| C7a | Fast Startup is switched off (`phases.c:1119`, `plat_win.c:995`). | none | 0 | 1-3 | LOW |
| C7b | The 1.8 GB download, as 20 gzip pieces from raw.githubusercontent, each with its own hash and resumed per piece (`phases.c:848-918`). | none | 0 | **≈ 330** at 50 Mbit/s (about 5.5 MB/s real) | HIGH (the biggest wait before the restart) |
| C7c | Unpack to the 5,333,057,536-byte image in `C:\AurOS`, hashed as it is written (`phases.c:920-955`). Peak disk use is 7.1 GB. | none | 0 | 45-60 SSD (CPU-bound) / 90-120 HDD | MEDIUM |
| C7d | **The whole image is read and hashed a second time** for the manifest (`phases.c:1355-1375`). | none | 0 | 25-35 SSD / 70-90 HDD | MEDIUM (wasted) |
| C7e | About 30 MB of staging files copied to `\EFI\AurOS`, plus BootNext (`phases.c:1503+`). | none | 0 | 5 | LOW |
| C8 | "Ready to restart". Primary button **Restart now**. Footer: "Save your work first. Closing instead puts everything back." Closing asks "Close without restarting?" with No as the default (`:3740-3750`). | Press Restart now. If she wandered off during the wait and the PC slept, she has to find the window again. | 5-15 | 0 | LOW |

### D. The restart (firmware → shim → GRUB → staging kernel)

| # | What she sees | Do | Attn | Wait | Severity |
|---|---|---|---|---|---|
| D1 | Windows shuts down, then the maker's logo. BootNext boots `\EFI\AurOS\shimx64.efi` → grub → the Canonical-signed kernel with the staging initramfs. Some OEM firmware (HP, some Lenovo) is known to ignore or reorder BootNext or OS-created entries; that is untested on real hardware ("Real PCs whose drive was resized: 0", `index.html:218`). | none | 0 | 45-75 SSD / 60-120 HDD | MEDIUM (unknown) |

### E. Staging: what she sees after the restart (`src/aurstage/install.c`, `src/aurscreen/screen.c`)

The screen shows "Installing AurOS", seven steps (`screen.c:74-80`), aurstage's latest sentence, a bar *when there is a percentage*, and from the shrink onward "Do not turn the computer off. It will restart by itself." (`screen.c:312-313`).

| # | Step on screen, and the sentence | What happens (bytes) | SSD (256 GB, 90 GB used) | HDD (1 TB 5400 rpm, 150 GB used) | Severity |
|---|---|---|---|---|---|
| E1 | "Checking this computer": "Getting ready. Nothing on this computer has been changed yet." | journal, NTFS state, encryption scan, SMART, power (`install.c:316-400`) | 10 s | 15 s | LOW |
| E2 | "checking the copy of AurOS on the Windows drive" | **A third full read and hash of the 5.3 GB image** from NTFS (`install.c:436`) | 30 s | 70-90 s | MEDIUM (wasted) |
| E3 | "measuring how small the Windows drive can get" | `ntfsresize --info` (`shrink.c:169-213`, 30-minute limit) | 10-30 s | 2-5 min | LOW |
| E4 | "**reading every sector of the space that would be reclaimed**" | Reads **every byte from ntfsresize's minimum size to the end of C:**, which is *all* the free space, not the 28 GB AurOS needs, 1 MiB at a time (`install.c:530`, `shrink.c:370-430`). **It is called with `progress = NULL`, so the screen shows no bar and no movement for the whole step.** It has a 4-hour limit (`shrink.c:389`). | about 140 GB at about 480 MB/s ≈ **5 min** | about 770 GB at 70-80 MB/s ≈ **2.7-3 h** (a 2 TB desktop hard disk with 300 GB used goes past the **4 h** limit and is *refused as "too slow"* after 4 hours) | **BLOCKER on HDD** (hours of a frozen-looking screen; she will switch it off) |
| E5 | "Saving the way back to Windows": "Saving this computer's Windows startup, so it can be put back." | Copies the ESP and the partition table into RAM and checks them (`install.c:558-565`) | 15 s | 30 s | LOW |
| E6 | "Making room on the Windows drive. This is the only part that cannot be undone. Do not turn the computer off." | `ntfsresize --size <smallest>`: **Windows is shrunk to ntfsresize's minimum** (`install.c:581`). Every cluster above the used-size mark is moved down. Then `plan_compute` gives *the whole gap* to AurOS (`plan.c`), and "Making AurOS fill the space it was given" (`install.c:819`). So **Windows is left essentially full** (only the 5.3 GB `C:\AurOS` image is reclaimable). This contradicts "About 28 GB" and "Windows keeps 8 GB" (`preflight.h:152-153`, `TRY-IT.md`). | moves about 20-40 GB ≈ **2-4 min** | moves about 60-100 GB at 20-40 MB/s ≈ **30-80 min** (`shrink.c` itself says "forty minutes") | **HIGH** (time; Windows unusable afterwards; "say No → back to Windows" lands her on a full C:) |
| E7 | "Keeping the way back on this computer, before anything else is written." | Writes and reads back the copy of the start-up files (`install.c:615-631`) | 10 s | 20 s | LOW |
| E8 | "Copying AurOS" | The **whole** root extent (about 4.8 GB, *all* blocks, not only used ones) is written from the NTFS file, then flushed, then *re-read from the source and from the disk* (`image.c:545-640`). In total about 5 GB written and about 15 GB read. | 60-90 s | 6-9 min (reading and writing on the same platter) | MEDIUM |
| E9 | "Setting up how it starts" | 512 MB boot area written and checked (`loader.c`) | 5 s | 20-30 s | LOW |
| E10 | "Checking AurOS works here": "Checking that this computer works under AurOS." | `probe_run_at` (`install.c:683`) mounts the new root and runs `modprobe -d /probe` on every modalias (`probe.c:48-70`), then counts `/sys/class/ieee80211` and wired carrier (`probe.c:297-345`). **(inferred)** The staging initramfs has **no firmware** (`build/staging:26`), and nothing points the kernel's firmware loader at `/probe/lib/firmware`. No `firmware_class.path` is set anywhere in the repo, despite `probe.h:20-22` claiming "loads ITS modules and ITS /lib/firmware". Intel (iwlwifi), Realtek (rtw88/rtlwifi), Qualcomm (ath10k), Broadcom and MediaTek Wi-Fi only register a radio *after* their firmware loads. So a **Wi-Fi-only laptop with no cable** gets `PROBE_NO_NETWORK` → `give_up(...)` **after the shrink**: "AurOS cannot get this computer online: it found no wireless and no network cable. The Windows drive is smaller but still works. Plug in a network cable and try again" (`install.c:684-699`). Every automated test uses `virtio-net` with a carrier (`tools/nosticktest.sh:363-368`), so none of them can see this. A retry then very likely stops in Windows at "Not enough free space on drive C:", because C: is now at its minimum. | 10-30 s | 20-60 s | **BLOCKER (inferred; most Wi-Fi-only laptops)** |
| E11 | "Finishing": "Writing the new layout." | Partition table committed in one sector, start-up entry added, second rescue copy kept, root grown to fill the gap | 20-40 s | 2-4 min | LOW |
| E12 | "AurOS is installed. Starting it now." | Hands over to AurOS in the same boot | 5 s | 5 s | LOW |
| E-stop | A red refusal: "**AurOS was not installed**", with "Turn this computer off and on again… To try again, **open AurBridge in Windows** and press Start installing." and "aurstage-report v1 verdict=… record=refused" (`install.c:72-97`; `docs/shots/staging-refused.png`). | She has to find the .exe (in Downloads), go through every page again, and know what "AurBridge" is (it's a component name, against EASY rule 8). | — | — | HIGH |

### F. First start of AurOS

| # | What she sees | Do | Attn | Wait | Severity |
|---|---|---|---|---|---|
| F1 | Boot. `firstboot.sh` grows the root (already grown), applies choices (locale-gen for her language), sets the theme, runs `ferry-detect` (`rootfs/usr/lib/auros/firstboot.sh`). There is **no login screen**: `aurshell.service` runs as `auros` with `PAMName=aurshell` (`rootfs/etc/systemd/system/aurshell.service`). | none | 0 | 30-45 SSD / 60-120 HDD | LOW |
| F2 | Welcome panel (`src/aurshell/welcome.c:376-445`): "**AurOS is on this computer**" — "Have a look around. Nothing has been decided yet." • "Right now, switching this computer on still starts Windows." • "That stays true until you press Yes." Buttons: "Yes, it all works" / "Let me look first" / "No, go back to Windows". | Press Yes. | 15-25 | 1-3 | LOW (well done) |
| F3 | "**AurOS starts from now on**", then "Bring my files across" / "Not now". | Press Not now (or Ferry). | 5-10 | 0 | LOW |
| F4 | Home (rail, `profiles/desktop.profile`: `shell_archetype="rail"`): "What would you like to do?" / "Pick one. You can always come back here." **Exactly 6 rows** (`HOME_TILES 6`, `layouts/rail.c:19`, paint `:178`, click `:411-433`) from the installed apps sorted by `rank_of` (`apps.c:393-412`): Web (Epiphany, rank 0), Your files (1), Documents (3), Notes (3), Photos (4), Music and video (5). **"Get more programs" (gnome-software, `PackageManager` → rank 6, renamed in `build/forge:396`) is 7th or lower, so it is not shown.** Rail has no "All programs", no search and no scrolling, and Settings has no programs section. Along the bottom: Help · Internet · Settings · Close this · Smaller · Bigger · Turn off (`foot.c:46-47`). | She cannot reach the store. | — | — | **BLOCKER** |
| F5 | The browser is **GNOME Web (Epiphany)**, not Firefox (`docs/results/build-all.txt:5541-5544`; `index.html:234`). | — | — | — | LOW-MEDIUM (some sites misbehave in WebKit) |

### G. Getting online and installing a program (assuming she could reach the store)

| # | What she sees | Do | Attn | Wait | Severity |
|---|---|---|---|---|---|
| G1 | "Internet" in the bottom bar → "Press the name of your wifi." → "Type the password for <name>". Advice: "It is usually printed on the bottom of your internet box." (`net.c:883-933`). **Ferry doesn't carry Wi-Fi passwords** (STATUS §8; `index.html:235`). | Find the router, read a 12-20 character random key, type it. | 120-360 | 10 | **HIGH** |
| G2 | "Get more programs" (gnome-software 46, PackageKit, apt only). **There is no Flatpak/Flathub**: `gnome-software-plugin-flatpak` is only suggested and not installed (`build-all.txt:5564`). Nothing configures a remote, although `apps.c:39` already looks in `/var/lib/flatpak/exports`. `rm -rf /var/lib/apt/lists/*` at build time (`build/forge`, `stage` end) means the **catalog is empty on first open** until PackageKit refreshes it (about 60-100 MB of lists and DEP-11 data). | Wait for the catalog. | 10 | 60-150 | MEDIUM |
| G3 | She searches for what she knows (Chrome, Zoom, WhatsApp, Spotify, Skype, Firefox, Thunderbird). They are **not in the Ubuntu archive**, or are snap-transitional packages on an image with snapd purged (`packages_exclude="snapd …"`). What she will find: LibreOffice, VLC, GIMP, games. | Search, read, choose. | 60-180 | 0 | **HIGH** (the program she wants usually isn't there) |
| G4 | Install. There is **no password prompt**: `49-auros.rules` grants `org.freedesktop.packagekit.package-install/remove/system-update` and gdebi to `auros` (`build/forge:1388-1396, 1452-1480`, `allow_user_install="yes"`). Risk: `unattended-upgrades` on first boot holds the dpkg lock → "Waiting for package manager lock". | Click Install. | 5 | 20-120 (plus any lock wait) | LOW / MEDIUM |
| G5 | Afterwards, **the new program doesn't appear in AurOS's list until the shell restarts**: `shell_scan_apps()` runs once at start-up (`main.c:731`) and a reload doesn't rescan (`main.c:1114-1140`). Even after a restart it only shows on rail Home if it ranks in the top 6. | Press "Open" inside Software, or never find it again. | 10 | 0 | **HIGH** |

---

## 2. Every refusal or stop the installer can produce

Kinds come from `fix_win.c` (`FIX_AUTO` = the "Fix these for me" button fixes it, `RESTART` = the installer restarts Windows and reopens via a scheduled task, `WAIT` = only she can fix it and the page notices by itself, `CANNOT` = dead end). Every `FIX_RESTART` turns into `CANNOT` if it's still there after the one reopen (`wizard.c:915-922`).

### 2a. Windows-side preflight (`src/aurbridge/preflight.c`, `sbdb.c`)

| id | Plain description (her line) | Which PCs hit it, and how common | Fix | Cost of the fix |
|---|---|---|---|---|
| `not-elevated` | "The installer wasn't given permission to make changes" | She pressed No on UAC, or uses a standard account: 2-5% | CANNOT ("Close it, open it again, and choose Yes") | 1 min, or impossible without the admin password |
| `firmware-bios` | "This PC starts Windows the old way (BIOS)" | Legacy/CSM installs: Windows 7 downgrades, DIY desktops, upgraded Windows 7 machines. **5-10%** of 2015-19 PCs, 40%+ of pre-2013 PCs | CANNOT ("Support is being built") | dead end |
| `secure-boot-ca` (R15) | "This PC is set to start only Windows". Card with a 7-step Windows path and a sketch of the firmware screen (`wizard-3c-*.png`) | Secured-core and third-party CA disabled: <1% of 2015-19 consumer PCs, 10-30% of 2021+ business laptops | CANNOT for her (it's a firmware setting) | 5-15 min for a helper; impossible for her |
| `secure-boot-newer` | "This PC needs a newer AurOS installer" | db trusts only the 2023 third-party CA, or the 2011 one is revoked: rare today, growing | CANNOT | dead end until a 2023-signed shim exists (STATUS 5.7) |
| `secure-boot-unsigned-build` | developer builds only | 0% for the release | — | — |
| `power-state-unknown` | "We can't tell if the charger is plugged in" | Broken ACPI reporting: <1% | WAIT (may never clear) | possibly a dead end |
| `not-on-ac` | "The charger isn't plugged in" | Laptops on battery: 30-50% at the moment of launch | WAIT | 10 s |
| `battery-low` | "The battery is low" — "Leave it charging for a few minutes" | **Below 50% even when plugged in** (`preflight.c:236-243`): 10-20%. **Worn or dead batteries that stay at 0-40% or say "plugged in, not charging"**: 5-10% of 7-10-year-old laptops | WAIT | charging 20→50% takes **15-40 min**; a dead battery is **stuck for ever** |
| `pending-reboot` | "Windows is waiting to restart to finish an update" | WU `RebootRequired` or CBS `RebootPending`: **10-25%** at any moment | RESTART (scheduled task `/rl highest`, reopens after sign-in) | 2-5 min SSD; **5-30 min** if updates install on the restart; a stuck key (2-5%) becomes CANNOT |
| `volume-dirty` | "Windows wants to check drive C: for errors" | 1-2% | RESTART | 2-5 min SSD; 10-60 min chkdsk on a large HDD |
| `dynamic-disk`, `storage-spaces`, `spanned-system-volume` | "Windows is on a multi-drive setup" / "…Storage Spaces pool" / "Drive C: is spread over more than one disk" | <1% at home | CANNOT | dead end |
| `system-disk-unknown` | "We couldn't tell which drive Windows is on" | RAID volumes, odd drivers: <1% | CANNOT | dead end |
| `smart-bad-sectors`, `smart-predict-failure` | "This drive is wearing out" / "…says it is about to fail" | Old HDDs with pending or uncorrectable sectors: 2-5% of 7-10-year-old HDDs | CANNOT | the drive needs replacing |
| `mbr-four-primaries` | "This drive has no room for another section" | MBR system disk with 4 primaries. Almost always BIOS too: <1% | CANNOT | dead end |
| `layout-unreadable`, `sector-size-unknown`, `system-disk-unreadable`, `no-disks`, `disk-unidentified` | "We couldn't read…" / "…identify one of this PC's drives" | Unusual storage drivers or virtual disks (VeraCrypt, emulators, some card readers with no bus type): <1-2% | CANNOT | dead end |
| `system-disk-removable` | **"Windows is running from a USB drive"** | Windows To Go (rare), **and eMMC laptops**: the system disk reports `BusTypeSd`/`BusTypeMmc`, which `bus_is_removable` treats as removable (`preflight.c:377-388, 787-792`). That covers 32/64 GB budget laptops (HP Stream, IdeaPad 100S/120S, Asus E2xx): **5-10%** of cheap 2015-19 laptops. The message is wrong for them. (They would fail on space anyway.) | CANNOT | dead end, with a false explanation |
| `removable-attached`, `removable-no-serial`, `recovery-stick-ambiguous` | "A USB drive or memory card is plugged in" — "Take it out. We carry on by ourselves." | Real USB sticks, external drives or SD cards: 10-20%, fixed by unplugging. **(inferred) Built-in USB card readers and printer card slots with no card in them**: Windows lists them as "Removable (No Media)" disks, `\\.\PhysicalDriveN` opens, and nothing filters out empty media (`preflight.c:802-870`). That is plausibly **10-25%** of laptops and desktops (Realtek USB readers, desktop front-panel multi-readers, all-in-one printers). There is nothing she can remove. | WAIT | 10 s for a real stick; **stuck for ever** for an empty built-in reader (inferred) |
| `bitlocker-system` / `bitlocker-other` | "Drive C: is encrypted" — "We switch encryption off. It takes a while; leave the PC on." | Device Encryption (TPM + Modern Standby, Microsoft account): **10-25%** of 2015-19 laptops on Windows 10, 40-60% of Windows 11 24H2 clean installs | AUTO (`manage-bde -off`, `fix_win.c:264-285`) | **SSD 256 GB: about 20-60 min; HDD 1 TB: about 2-6 h**. She must leave the PC on with the wizard open (it re-checks every about 3 s and carries on by itself). |
| `bitlocker-unknown`, `fde-unreadable`, `fde-not-checked`, `fde-check-failed` | "We couldn't check whether drive C: is encrypted" | rare, <1% | RESTART once, then CANNOT | 2-5 min, or a dead end |
| `fde-third-party` | "Another encryption program protects this drive" | McAfee/Symantec/VeraCrypt system encryption: <1% at home | CANNOT ("Switch it off in that program") | hours, with a helper |
| `fde-managed` | "This PC's encryption is managed by an organisation" | ex-corporate laptops: 1-3% of refurbished PCs | CANNOT | dead end |
| `system-not-ntfs` | "Drive C: isn't formatted the usual way" | ~0% | CANNOT | — |
| `space-unmeasurable` | "We couldn't measure the free space on drive C:" | rare | RESTART | 2-5 min |
| `insufficient-space` | "Not enough free space on drive C:" | Needs `offline_shrinkable − 8 GB ≥ 28 GB` (`preflight.c:1173-1185`), **plus** 7.1 GB room for download and unpack, rechecked at Start installing (`phases.c:489-505`). That is about **≥ 43 GB effectively free**. 128 GB SSD laptops: 30-50% fail. 64 GB eMMC: 100% fail. Overall **15-25%** | AUTO: Recycle Bin, temp folders, `cleanmgr /sagerun` with up to **15 min** for Update Cleanup (`fix_win.c:188-256`). Then "Move big files (videos, games) off C:" | 1-15 min. Usually frees 1-10 GB. If still short, a **dead end for her** |
| `esp-missing` | "This PC's start-up area isn't on the Windows drive" | multi-disk desktops installed with another disk attached: 1-3% | CANNOT | dead end |
| `esp-unreadable` | "We couldn't read this PC's start-up area" | rare | CANNOT | dead end |
| `esp-low-space` | "This PC's start-up area is too full" | 100 MB OEM ESPs filled with fonts and vendor tools (needs about 33 MB): 2-5% | CANNOT (nothing tries to clean it up) | dead end |
| *warnings, not stops* | `intel-rst`, `low-ram`, `smart-reallocated`, `dynamic-disk-other`, `storage-spaces-other`, `fde-container-software`, `fast-startup` (INFO, fixed by the install) | — | — | see `intel-rst` below |

**Combined:** on a random 2015-19 consumer PC I estimate **35-55%** hit at least one stop. Roughly half of those clear with one press or by plugging something in. Many of the rest cost tens of minutes to hours (BitLocker, charging, pending updates) or are permanent (BIOS, card reader, eMMC, space on 128 GB).

### 2b. After "Start installing", still in Windows (`phases.c`)

| Stop | When | Cost |
|---|---|---|
| "there is not enough room on the Windows drive to download AurOS: it needs N MB free…" (`phases.c:497-503`) | Free space is below image + pieces (7.1 GB) | The pages are lost; back to Disk Cleanup |
| Fast Startup could not be switched off (`plat_fast_startup_off`) | rare | dead end |
| "part N of AurOS could not be downloaded intact… press Start installing again" (`phases.c:905-912`) | flaky Wi-Fi, **the PC going to sleep** (no `SetThreadExecutionState`), proxy or antivirus HTTPS inspection | minutes; what already arrived is kept |
| "the downloaded AurOS does not match what this installer expects" (`phases.c:941-953`) | a publishing mistake | dead end until republished |
| ESP room re-check in phase 3 (`esp_room`) | ESP filled up in between | dead end |

### 2c. After the restart (`src/aurstage/install.c`). The red screen "AurOS was not installed"

**Above the line** (nothing changed; it costs her the restart, plus the whole wizard again):
- `bad-journal`, `wrong-disk`. **Intel RST "RAID On" with NVMe remapping**: Dell 2016-2020 NVMe laptops and HP Optane models. The Linux kernel can't see a remapped NVMe disk, so this fails here. Preflight only *warns* (`preflight.c:1660-1673`). About **5-10%** of 2015-19 laptops (inferred: the disk is missing, so the journal can't match).
- `sector-size-unknown`, `not-gpt`, `moved`.
- NTFS state (`hibernated`, `dirty`, `log`): e.g. if Windows chose a hybrid shutdown. Unlikely because this is a restart.
- `ntfs-unsure`, `third-party-encryption`, `encryption-unsure`, `drive-failing`.
- `on-battery`: she unplugged during the hours-long run.
- `windows-unreadable` (ntfs3 mount), `no-image`, `image-damaged`, `no-boot-area`, `windows-still-open`, `cannot-size-rescue`, `no-memory`, `no-table`.
- `cannot-measure`: ntfsresize refuses, e.g. a volume with bad clusters or unsupported features.
- `bad-sectors`: **including the 4-hour limit on large HDDs**.
- `no-room`, `rescue-capture-failed`.

**Below the line** (Windows already shrunk):
- `shrink-could-not-start`, `shrink-failed`, `no-room-after-shrink`, `saved-copy-failed`, `disk-not-writable`, `cannot-arm`, `write-failed`, `boot-write-failed`.
- **`no-network` (inferred: most Wi-Fi-only laptops, see E10)**, `no-drivers`, `unmountable`.
- Commit or boot-entry failures.
- Every one of these leaves **Windows at its minimum size with essentially no free space**. A retry then very likely stops in Windows at `insufficient-space`.

---

## 3. Wall-clock estimate, typical 2015-2019 laptop, 50 Mbit/s

This is the happy path: no preflight stop, a network cable plugged in (or the Wi-Fi bug fixed), and the store reachable. Her attention and the waits are combined.

| Phase | 2017 i5, 8 GB, 256 GB SATA SSD, 90 GB used | 2015 i3, 4-8 GB, 1 TB 5400 rpm HDD, 150 GB used |
|---|---|---|
| Website: understand, scroll 11 screens, tick 3 boxes, download | 3-8 min | 3-8 min |
| Browser "not commonly downloaded", SmartScreen, UAC | 1-2 min | 1-2 min |
| Wizard pages (Welcome → Ready, about 1,100 words, 3 ticks, type AGREE) | 4-6 min | 4-6 min |
| Re-check + Fast Startup | 0.3 min | 0.5 min |
| Download 1.8 GB | 5.5 min | 5.5 min |
| Unpack + hash 5.3 GB | 0.8-1 min | 1.5-2 min |
| Second full hash (manifest) | 0.5 min | 1.2-1.5 min |
| Restart (shutdown, POST, shim, grub, staging boot) | 1-1.3 min | 1.5-2 min |
| Gate + third full hash of the image + ntfsresize --info | 1 min | 4-7 min |
| **Surface test of all free space** (no progress shown) | **≈ 5 min** | **≈ 2.7-3 h** |
| Rescue capture | 0.3 min | 0.5 min |
| **Shrink to the minimum** | 2-4 min | **30-80 min** |
| Copy 4.8 GB + full read-back | 1-1.5 min | 6-9 min |
| Boot area, probe, commit, grow | 1 min | 3-5 min |
| AurOS first boot to the welcome | 0.5-0.8 min | 1-2 min |
| Welcome answers | 0.5 min | 0.5 min |
| Wi-Fi password (find the router, type the key) | 2-6 min | 2-6 min |
| Store: open, catalog refresh, search, install | 3-6 min | 4-8 min |
| **Total** | **≈ 33-50 min** | **≈ 4-5 h** |

Variants:
- 1 TB HDD with BitLocker: add 2-6 h of decryption.
- 500 GB HDD with 100 GB used: the surface test takes about 80 min and the shrink about 20-50 min, so the total is about **2-2.5 h**.
- NVMe SSD: the surface test takes about 1-2 min, so the total is about 28-45 min.
- **Hard floor today** (no reading at all, everything instant except physics, SSD): download 5.5 + unpack and two hashes 1.5 + restarts 2 + surface 5 + shrink 2 + copy 1.3 + rest 2 ≈ **19 min**. So **under 10 minutes is impossible with the current design at 50 Mbit/s**, even before counting a single word she reads.

---

## 4. The top 15 changes, ranked by impact divided by effort

The order is opinionated: unblocking the default path comes first, then cutting the long waits, then cutting her reading.

1. **Make the network check after the restart load the image's Wi-Fi firmware, and do it before the shrink.** (BLOCKER, small effort.)
   - Before the modalias walk, write `/probe/lib/firmware` into `/sys/module/firmware_class/parameters/path`.
   - After `modprobe`, poll `/sys/class/ieee80211` for up to about 10 s; registration is asynchronous.
   - Move `probe_run_at` ahead of the shrink by probing the image's root extent read-only from the file on NTFS (loop at an offset). Then "no network" is a refusal above the line, not a give-up below it.
   - Add a test with a firmware-requiring virtual Wi-Fi (or an iwlwifi laptop on a bench) and no cable.
   - Files: `src/aurstage/probe.c`, `probe.h`, `install.c:675-700`, `tools/probetest.sh`, `tools/nosticktest.sh`.

2. **Put "Get more programs" on the default Home, and make newly installed programs appear.** (BLOCKER, small effort.)
   - In rail, give the store a fixed last row ("Get more programs"), plus an "Everything else" row that opens a scrollable list of all apps.
   - Rescan desktop entries on inotify for `/usr/share/applications`, `/usr/local/share/applications`, `~/.local/share/applications` and the Flatpak export dirs, or at least on every reload.
   - Files: `src/aurshell/layouts/rail.c` (`HOME_TILES`, `l_click`, paint), `src/aurshell/apps.c` (`rank_of`, a "pinned store" flag), `src/aurshell/main.c:731, 1114`. Also apply it in `tiles.c`.

3. **Stop refusing PCs for an empty card reader, and stop calling eMMC a USB drive.** (BLOCKER for a slice of PCs, small effort.)
   - Skip removable disks with no media: `IOCTL_STORAGE_CHECK_VERIFY2` fails, or `IOCTL_DISK_GET_LENGTH_INFO` fails or returns 0.
   - Downgrade "a USB stick is plugged in" to a warning. The journal already pins the target disk's serial and model, and the staging side only writes the disk the journal names, so physically unplugging isn't what keeps this safe.
   - Treat `BusTypeSd`/`BusTypeMmc` on the *system* disk as internal eMMC.
   - Files: `src/aurbridge/preflight.c:377-388, 787-870`, `fix_win.c:42-50`.

4. **Shrink only what AurOS needs, and surface-test only that region, with progress.** (BLOCKER on HDD, medium effort.)
   - Choose the Windows target as `max(smallest, current − AurOS_share)`, where `AurOS_share = clamp(40 GB … 50% of reclaimable)`, and leave Windows at least 20 GB (or 25%) free.
   - Pass that target to `shrink_do` and the surface test, not `sp.smallest_bytes`.
   - Pass a real progress callback (now `NULL` at `install.c:530`).
   - On an SSD (no seek penalty), rely on SMART plus the write-then-read-back that already exists.
   - This turns the HDD run from hours into about 20-30 minutes, leaves Windows usable, and makes the wizard's "About 28 GB" true.
   - Files: `src/aurstage/install.c:519-600`, `plan.c`, `shrink.c`, `main.c` (dry run), `src/aurbridge/wizard.c:2779`.

5. **Give her a front door on the website.** (BLOCKER, small effort.)
   - The first screen should have one sentence, one big "Get AurOS for this Windows computer" button, and "Open this page on the Windows computer you want to change".
   - Add a 3-picture strip of what Windows will say (Keep anyway → More info → Run anyway → Yes), and the honest time for *her* PC type.
   - Move the LBA/partition storytelling, the power button, the SMART table and the defect list to `how-it-works.html`, which is already a redirect stub.
   - Keep the test-build checkboxes only while the build is a test build, and don't make the narrative the only route to the download.
   - Files: `website/index.html`, `website/assets/drive.js:685-693`, `drive.css`, `website/how-it-works.html`, `website/guide.html`.

6. **Sign the installer** (Azure Trusted/Artifact Signing or OV). (BLOCKER, no code, money.)
   - This removes Edge's "isn't commonly downloaded", SmartScreen's Run anyway and Smart App Control's hard block, and shortens Avast's hold.
   - Then submit the file to the antivirus vendors.
   - Files: `build/sign`, `docs/SIGNING.md`, `docs/RELEASE.md`.

7. **Start the download the moment the check passes, keep the PC awake, and hash once.** (HIGH, medium effort.)
   - Split phase "Get AurOS" out of `install_begin`, and run it in the background while she reads Welcome → Ready. It only writes `C:\AurOS`, which closing the window already cleans up.
   - Call `SetThreadExecutionState(ES_CONTINUOUS|ES_SYSTEM_REQUIRED)` for the whole download.
   - Compute the root/ESP manifest hashes *during* the inflate (the extents are known from the first 16 KiB) instead of re-reading 5.3 GB (`phases.c:1355-1375`).
   - In staging, trust the manifest plus the per-chunk read-back instead of a third full hash (`install.c:436`).
   - This hides about 6 min of waiting behind her reading.
   - Files: `src/aurbridge/wizard.c` (`install_begin`, `do_primary`), `phases.c`, `plat_win.c`, `src/aurstage/image.c`, `install.c`.

8. **Cut the wizard to three screens and zero typing.** (HIGH, small-to-medium effort.)
   - Screen 1: Welcome, with the check running automatically.
   - Screen 2: "Here's what will happen", no more than 80 words, one checkbox "Keep Windows; put AurOS beside it", and the honest time.
   - Screen 3: Installing.
   - Replace "Type AGREE" with one large "Yes, install AurOS" button; the second checkbox and the re-check already prove intent.
   - Replace the backup checkbox with an offer ("Copy my Photos and Documents to a USB stick first?") or a plain reassurance, since Windows and its files stay.
   - Name the drive by role and size ("this computer's own drive, 256 GB"), not the model.
   - Fix the false lines: `:1801` "you pick", `:2779` "About 28 GB", `:1208` "About 5 GB is downloaded", `:2814` "About 40 minutes", `:1216` "sleep", `:1220` "Only now… permanent", `:1846` "coming up next".
   - Raise muted text contrast to at least 4.5:1.
   - Files: `src/aurbridge/wizard.c` (`page_backup`, `page_consent`, `page_ready`, `PHASES`, `nav_allowed`, the palette).

9. **Carry her Wi-Fi across.** (HIGH, medium effort.)
   - The installer already runs elevated, so `WlanGetProfile(..., WLAN_PROFILE_GET_PLAINTEXT_KEY)` returns the connected network's key.
   - Hand it to staging through the journal (not world-readable `choices.conf` on the ESP), and write a 0600 NetworkManager keyfile into the new root before commit.
   - The first boot is then online with no typing, and the network probe can test the actual network.
   - Files: `src/aurbridge/plat_win.c`, `phases.c` (journal and `write_choices`), `src/aurstage/journal.c`, `install.c`, `rootfs/usr/lib/auros/choices.sh`, `src/ferry/`.

10. **Relax the battery rule.** (HIGH for a slice, tiny effort.)
    - Require AC only (`not-on-ac`), or require at least 20%.
    - Never block "plugged in, not charging" or a reading of 0% on AC: a dead battery on mains is exactly as safe as a desktop.
    - Files: `src/aurbridge/preflight.c:181-243`, `fix_win.c:39-41`, staging `power_ok`.

11. **Refuse Intel RST NVMe remapping in Windows, before the download and the restart.** (HIGH for a slice, small effort.)
    - Detect `iaStorAC`/`iaStorAVC`/`iaStorVD` with an NVMe child or Optane, and say it plainly once.
    - Or ship VMD support where it applies (11th gen and later) so those PCs work.
    - Files: `src/aurbridge/preflight.c:1660-1673`, `fix_win.c`, `wizard.c` CHK row.

12. **A store that has what she wants, ready on first open.** (HIGH, medium effort.)
    - Install `gnome-software-plugin-flatpak` plus `flatpak`, and preconfigure the Flathub remote, so Chrome, Zoom, Spotify, WhatsApp clients and Firefox appear.
    - Extend `49-auros.rules` with `org.freedesktop.Flatpak.app-install`, `runtime-install`, `modify-repo` (or install system-wide via the polkit grant).
    - Add `~/.local/share/flatpak/exports/share/applications` to `APP_DIRS`.
    - Ship the apt lists and DEP-11 cache in the image (don't `rm -rf /var/lib/apt/lists/*`), or pre-warm the catalog on first network.
    - Defer `unattended-upgrades` for the first hour so PackageKit doesn't wait on the lock.
    - Files: `build/forge` (`stage_store`, the polkit section, apt clean), `profiles/desktop.profile`, `src/aurshell/apps.c:36-41`.

13. **Be honest about time, and show movement during every long step.** (MEDIUM, small effort.)
    - Detect SSD or HDD in preflight (`StorageDeviceSeekPenaltyProperty`) and show "about 30 minutes" or "about 2 hours" before she says yes (EASY rule 9).
    - Show "about N minutes left" from measured throughput on the wizard progress page and on `aurscreen`.
    - Make the website's "An evening" and the wizard's "About 40 minutes" agree with reality.
    - Files: `preflight.c`, `wizard.c:2814`, `src/aurscreen/screen.c` (pct, ETA), `install.c` (progress for surface/verify), `website/index.html:145`.

14. **Write and download less.** (MEDIUM, medium effort.)
    - Copy only the used ext4 blocks of the root (read the block bitmap, or e2image-style), and verify only those.
    - Shrink the image's root to `resize2fs -M` size and the ESP from 512 MB toward what's needed.
    - Compress with zstd -19 or xz instead of gzip (the download goes from about 1.8 GB to about 1.2 GB, roughly 2 min at 50 Mbit/s).
    - Move hosting from a git branch to Releases/CDN (STATUS 7.3), so many users at once aren't throttled.
    - Files: `build/mkimage`, `build/aurbridge` (pieces), `src/aurbridge/inflate.c` (or a zstd decoder), `src/aurstage/image.c:545-640`.

15. **Make the "Fix these for me" waits walk-away-able, and the red screens actionable.** (MEDIUM, small effort.)
    - For BitLocker decryption, the hour-long Disk Cleanup and charging: keep the PC awake, show a percentage (`manage-bde -status` conversion %), and say "you can leave it; this window will carry on by itself".
    - For `pending-reboot`, also restart automatically when the timer allows.
    - On the staging red screen: no "AurBridge", no `verdict=` as the main text. Tell her "When Windows starts, the AurOS installer opens by itself", by arming the same scheduled task `fix_resume_arm` already creates before the restart, so a retry needs no hunt for the .exe.
    - Files: `src/aurbridge/fix_win.c`, `wizard.c` (`bg_recheck_tick`, `page_blocked`), `phases.c` (arm the resume task in phase 3), `src/aurstage/install.c:72-97`, `src/aurscreen/screen.c:231-236`.

Not in the top 15 but worth doing:
- AurOS's own text is English-only even when Windows was Spanish or German (STATUS 5.6).
- Firefox instead of Epiphany (a builder network policy issue, STATUS 7.1).
- The 2011 Microsoft UEFI CA expired in June 2026, so a 2023-signed shim path is needed soon (STATUS 5.7).
- Avoid leaving Windows at its minimum even when a later step gives up, so a retry can pass preflight.

---

## Appendix: evidence pointers

- Rendered website (current): `index-desk-fold.png`, `index-desk-scroll1.png`, `index-desk-full.png`, `index-desk-backup-ticked.png`, `index-phone-*.png`. The download button is at y≈8638 of 8946 px (desktop) and y≈11531 of 11930 px (phone).
- Rendered wizard (current code, Wine; the fallback serif font in titles is Wine's): `wizard-1-welcome.png`, `wizard-3-blocked.png`, `wizard-3c-blocked-secure-boot-picture.png`, `wizard-4a-backup-empty.png`, `wizard-5a-consent.png`, `wizard-9-ready.png`, `wizard-10-progress.png`.
- Staging screens: `docs/shots/staging-installing.png`, `docs/shots/staging-refused.png`.
- Key code lines:
  - `src/aurstage/install.c:527-546` (surface test region, progress NULL), `:581` (shrink to the minimum), `:683-699` (probe after the shrink), `:819` (fill the gap)
  - `src/aurstage/probe.c:48-70, 261-293, 297-345`
  - `build/staging:26-31`
  - `src/aurshell/layouts/rail.c:19, 178, 411-433`
  - `src/aurshell/apps.c:393-412, 490-606`
  - `src/aurshell/main.c:731, 1114-1140`
  - `build/forge:265-270, 396`, polkit `1452-1480`
  - `src/aurbridge/preflight.c:206-243, 377-388, 787-870, 1173-1185, 1660-1673`
  - `src/aurbridge/fix_win.c:14-134`
  - `src/aurbridge/phases.c:848-955, 1355-1375, 489-505`
