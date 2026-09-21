<div align="center">

# AurOS

**A Linux desktop that a non-technical person installs on their old Windows PC
by downloading one file.**

</div>

![AurOS booted](docs/shots/booted-desktop-secureboot.png)

<sub>Not a mock-up — a screendump of AurOS running in a VM with Secure Boot
**enabled**, `aurshell` painting directly to DRM/KMS.</sub>

---

## What this is

Four things that have to work as one:

| | | |
|---|---|---|
| **AurOS** | the distribution | our init, package manager, desktop shell and theme engine, on an Ubuntu LTS base |
| **AurBridge** | the Windows `.exe` | preflight, consent, repartition, write, boot handoff — the only part that can destroy someone's data |
| **Ferry** | first-boot migration | pulls files and settings off the still-intact Windows partition |
| **Forge** | the customization layer | one declarative profile → a branded, locked-down, localized build |

The goal, stated as a testable contract:

> A person opens a website, downloads one file, double-clicks it, reads and
> agrees to what will happen, and afterwards is sitting in a working,
> good-looking Linux desktop with their own files already there.

## Where it actually is

**Working and verified:**

- **Boots with Secure Boot on.** OVMF with Microsoft's keys → Microsoft-signed
  shim → Canonical-signed GRUB → kernel → `aurshell`. The shim we ship is
  dual-signed (Microsoft UEFI CA **2011** *and* 2023, checked with `sbverify`),
  so it also boots firmware that predates the 2023 CA — which is most of the
  old hardware this exists for.
- **The desktop shell runs**, painting straight to DRM/KMS. No X11, no Wayland
  compositor, no Mesa in that path.
- **Theme engine** — one file drives wallpaper, bar, palette, terminal, TTY,
  GTK and bootloader. Four themes, including a light one.
- **Procedural wallpapers** — generated from the theme, so a reskin never
  strands a stale photo and the image carries no JPEGs.
- **From-scratch TrueType rasterizer** and a PNG encoder with its own DEFLATE.
- **AurBridge preflight** — a real Windows `.exe` that refuses to run on a
  machine it cannot make safe.
- **Recovery capture/restore** — destroy-and-restore tested: wipe a disk's
  partition table and its backup, restore byte-identical, Windows BCD intact.
- **Ferry** — read-only NTFS, OneDrive placeholder classification, Firefox
  profile transplant, NetworkManager import, CLDR timezone mapping.
- **Website** — five pages including an honesty page about what can go wrong.

**Not built yet:** AurBridge's destructive phases (shrink, write, boot
handoff) are specified in `docs/AURBRIDGE.md` and deliberately unwritten until
the recovery path is wired to them. **Nothing ships until failing an install
at every phase has been tested on purpose.**

**Not a code problem:** a legal entity, an OV code-signing certificate, and
insurance are prerequisites to the first public download. See
`docs/research/signing-trust.md`.

## Try it

```sh
sudo ./build/forge build desktop     # bootstrap + package + theme an image
sudo ./build/mkimage desktop         # → out/auros-desktop.img, bootable
```

Boot it with Secure Boot on:

```sh
cp /usr/share/OVMF/OVMF_VARS_4M.ms.fd /tmp/vars.fd
qemu-system-x86_64 -machine q35,smm=on -m 3072 \
  -global driver=cfi.pflash01,property=secure,value=on \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.secboot.fd \
  -drive if=pflash,format=raw,file=/tmp/vars.fd \
  -drive file=out/auros-desktop.img,format=raw,if=virtio \
  -device virtio-gpu-pci
```

Preview the desktop without booting anything:

```sh
aurshell --conf /etc/auros/shell.conf --png /tmp/desktop.png --size 1600 900
```

## Reskin it

This is the part built to be handed to someone else. Edit one file:

```sh
aurora new midnight --from nocturne   # scaffold
$EDITOR /usr/share/auros/themes/midnight.theme
aurora set midnight                   # wallpaper, bar, terminal, TTY, GRUB
```

`aurora` renders every template from the theme and supports a colour algebra,
so derived shades are never hand-written:

```
@accent@              #7DD3C0
@accent|lighten:20@   lighter by 20%
@accent|mix:bg:70@    70% toward the background
@accent|on@           a readable foreground to sit on the accent
@accent|rgba:0.4@     rgba(125,211,192,0.40)
```

Themes inherit, so a variant states only what it changes. See
[`docs/THEMING.md`](docs/THEMING.md).

## Rebrand and lock it down

A profile is the whole customization surface in one file — branding, theme,
locale, app set, policy. What a school ships is data, not code:

```sh
./build/forge build school-kiosk     # one browser, no installs, no console
./build/forge build multilingual     # four languages, input methods, fonts
```

## Repository

```
build/     forge (image builder) · mkimage (bootable image) · aurb (packages)
src/
  aurora/    the theme engine
  aurshell/  the desktop shell — DRM/KMS, rasterizer, compositing
  aurinit/   PID 1 and aurctl
  aurbridge/ the Windows installer (preflight built; phases specified)
  ferry/     first-boot migration off Windows
  recovery/  capture and restore a machine's boot state
  common/    theme parser · wallpaper renderer · font · PNG
profiles/  desktop · school-kiosk · multilingual
themes/    nocturne · synthwave · sandstone · moss + templates
docs/      PLAN · AURBRIDGE · THEMING · FERRY · research/
website/   the download site
```

## Read this before trusting it with a disk

[`docs/research/red-team.md`](docs/research/red-team.md) is an adversarial
review of this product. It is not marketing. It opens by calling the concept
*"a consumer-grade disk-destruction tool with a wizard on it"* unless every
hard stop is implemented, and it is right. Every hard stop it names is a
requirement on AurBridge, and the ones that are implemented carry their
register id in the code.

## Licence

MIT. See [`LICENSE`](LICENSE).
