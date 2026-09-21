# ═══════════════════════════════════════════════════════════════════
#  AurOS Profile — Desktop (the default)
#
#  A profile is the whole customization surface in one file. Forge
#  reads it and produces a bootable image: branding, theme, language,
#  the app set, and what users are allowed to change.
#
#  This is the file a school or an organisation edits. Everything an
#  administrator could reasonably want to control is here, and nothing
#  here requires touching code.
# ═══════════════════════════════════════════════════════════════════

profile_id="desktop"
profile_name="AurOS Desktop"
profile_description="The standard AurOS experience for a personal computer."

# ── Base ───────────────────────────────────────────────────────────
# Ubuntu LTS. Chosen over Debian for one decisive reason: Canonical's
# shim-signed / grub-efi-amd64-signed / linux-image-generic chain is
# already Microsoft-signed, so Secure Boot works with no shim-review and
# no MOK prompt. See docs/research/signing-trust.md.
base_suite="noble"
base_mirror="http://archive.ubuntu.com/ubuntu"
base_components="main restricted universe multiverse"

# ── Identity ───────────────────────────────────────────────────────
brand_name="AurOS"
brand_short="auros"
brand_url="https://auros.example"
os_version="0.1.0"
os_codename="Nocturne"

# ── Look ───────────────────────────────────────────────────────────
theme="nocturne"
wallpaper_style=""        # empty = whatever the theme says

# ── How the computer works (docs/SHELLS.md) ────────────────────────
# The single most consequential line in this file. It decides how a
# person reaches a different thing, which is what actually makes a
# computer feel easy or hostile. Colours are cosmetic next to it.
#
#   rail       everything open sits in a row; nothing can hide
#   tiles      a page of big buttons; one thing fills the screen
#   locked     only the apps the owner chose; nothing else exists
#   taskbar    a bar of open windows along the bottom; they overlap
#   dock       favourites always in the same place along the edge
#   workbench  windows divide the screen; keyboard-driven
#
# All six are built into every image. This picks the default; a user
# can try another with `aurshell --shell`, and an administrator pins
# it by leaving allow_settings_change="no".
shell_archetype="rail"

# ── Locale ─────────────────────────────────────────────────────────
locale="en_US.UTF-8"
extra_locales=""          # e.g. "fr_FR.UTF-8 zh_CN.UTF-8"
timezone="UTC"
keyboard_layout="us"
keyboard_variant=""

# ── Packages ───────────────────────────────────────────────────────
# Hardware enablement. This is the line that makes WiFi and graphics
# work on a ten-year-old laptop, and it is the single strongest reason
# to sit on an existing base rather than rebuild one.
packages_hardware="linux-image-generic linux-firmware
                   network-manager wireless-tools wpasupplicant
                   pciutils usbutils"

# Boot chain. Signed, so Secure Boot does not have to be disabled.
packages_boot="shim-signed grub-efi-amd64-signed grub-efi-amd64
               efibootmgr os-prober"

# Minimum userland for a system that can repair and explain itself.
packages_base="systemd-sysv dbus udev sudo less nano
                   initramfs-tools
               ca-certificates curl wget
               e2fsprogs dosfstools ntfs-3g gdisk parted
                   cloud-guest-utils util-linux
               zstd xz-utils bzip2
               iputils-ping openssh-client
               fonts-inter fonts-jetbrains-mono"

# The desktop. Kept deliberately small; aurshell provides the shell.
packages_desktop="xwayland libinput10 libdrm2 libgbm1 mesa-vulkan-drivers
                  pipewire pipewire-pulse wireplumber
                  fontconfig"

# What the user actually opens.
#
# Ubuntu ships Firefox and Chromium ONLY as snap transitional packages,
# so purging snapd (below) removes the browser with them. A desktop
# without a browser is not a product, so the browser comes from a real
# apt repository instead.
#
#   mozilla-apt  Mozilla's official .deb repo (packages.mozilla.org)
#   snap         Ubuntu's default; requires snapd, slow on old hardware
#   none         headless / appliance builds
browser="firefox"
browser_source="mozilla-apt"

packages_apps=""

packages_extra=""
packages_exclude="snapd ubuntu-advantage-tools popularity-contest"

# ── Policy / lockdown (the schools + organisations story) ──────────
kiosk_mode="no"           # yes = single-app, no desktop, no shell access
allowed_apps=""           # empty = everything installed is allowed
blocked_apps=""
allow_user_install="yes"  # may users install software?
allow_settings_change="yes"
allow_theme_change="yes"
allow_tty="yes"           # Ctrl-Alt-F2 to a console
auto_login="no"
default_user="auros"

# ── Fleet ──────────────────────────────────────────────────────────
enrollment_url=""         # optional MDM / fleet check-in endpoint
update_channel="stable"
telemetry="off"           # off | anonymous — never on by default
