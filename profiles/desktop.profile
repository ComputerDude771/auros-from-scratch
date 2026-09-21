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
#
# polkitd is here and not in packages_base because it is what decides
# whether the desktop is allowed to change the network at all. Without
# it the Internet button in the band appears, presses, and refuses --
# a control that looks like a control and is not, which is the exact
# failure this product keeps finding in itself.
packages_hardware="linux-image-generic linux-firmware
                   network-manager wireless-tools wpasupplicant
                   polkitd
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
               fonts-inter fonts-jetbrains-mono
               fonts-paratype"

# The desktop. Kept deliberately small; aurshell provides the shell.
# What an application needs before it can put a window on the screen.
# Every line here was added because something concrete failed without
# it, not because it seemed likely to be useful.
#
#   dbus-user-session   the first hard failure: a real browser will not
#                       start at all without a session bus, and says so
#                       as "Cannot autolaunch D-Bus without X11 $DISPLAY"
#                       -- an error about X11 on a machine that has none
#   fonts-*             an application draws its own text; the shell's
#                       fonts are the shell's
#   ca-certificates     otherwise every https page is a security error
#   libgl1-mesa-dri     llvmpipe. The compositor is CPU-only by design,
#                       but a toolkit that cannot create ANY GL context
#                       aborts rather than falling back
#   *-icon-theme,       GTK and Qt applications look broken, or refuse
#   gsettings-*,        to start, without their schemas, icons and mime
#   shared-mime-info    database. This is not decoration.
#
# mesa-vulkan-drivers was removed rather than lost: it is ~100 MB, and
# nothing in this product can use it. The compositor composites on the
# CPU by design, and the machines this exists to rescue have no Vulkan
# driver worth loading.
packages_desktop="xwayland libinput10 libdrm2 libgbm1
                  pipewire pipewire-pulse wireplumber
                  fontconfig fonts-dejavu-core fonts-liberation2
                  dbus-user-session dbus-daemon ca-certificates
                  libgl1-mesa-dri libegl-mesa0 libglx-mesa0
                  adwaita-icon-theme hicolor-icon-theme
                  gsettings-desktop-schemas shared-mime-info xdg-utils"

# What the user actually opens.
#
# Ubuntu ships Firefox and Chromium ONLY as snap transitional packages,
# so purging snapd (below) removes the browser with them. A desktop
# without a browser is not a product, so the browser comes from a real
# apt repository instead.
#
#   mozilla-apt  Mozilla's official .deb repo (packages.mozilla.org)
#   archive      a real .deb from Ubuntu's own archive
#   snap         Ubuntu's default; requires snapd, slow on old hardware
#   none         headless / appliance builds
#
# browser_fallback is used when the preferred source cannot be reached
# from the build host -- a corporate proxy, an air-gapped builder, a
# blocked domain. Shipping a desktop with no browser because of the
# network the BUILDER was on is not a trade-off anyone chose, and the
# fallback is a real browser rather than an apology.
browser="firefox"
browser_source="mozilla-apt"
browser_fallback="epiphany-browser"

# ── Opening things, and getting more things ────────────────────────
#
# Software arrives here the way it arrives on any other Linux machine:
# from the archive, or as a .deb downloaded from a website. There is no
# AurOS-only walled garden, and there is no attempt to run .exe files --
# a .exe is a Windows program and nothing on Linux runs one. What IS
# here is the normal path, made pressable:
#
#   thunar     her files, in a window. Double-clicking something opens
#              it with whatever claims that kind of file, which is what
#              makes "make files openable" true rather than a plan.
#   gdebi      double-click a downloaded .deb and it installs, with a
#              screen first saying what it is. This is the closest thing
#              Linux has to the .exe habit she has had for twenty years,
#              and it is how Google Chrome is actually installed.
#   ristretto  pictures      atril     PDFs
#   mousepad   text          xarchiver zip files
#
# The viewers are the light ones (XFCE and MATE) rather than the GNOME
# ones: ristretto is 8 packages where eog is 14, and this is a machine
# from 2013.
packages_files="thunar thunar-volman gvfs gvfs-backends udisks2
                gdebi
                ristretto atril mousepad xarchiver
                shared-mime-info desktop-file-utils xdg-user-dirs"

# The browse-and-install store. Heavier than everything above put
# together -- 35 packages and a background service -- so it is a knob
# rather than an assumption, and a build for a slow machine can drop it
# and still install software by downloading it.
#   gnome-software  the usual Ubuntu store, backed by packagekit
#   none            the archive is still there; .deb files still install
software_store="gnome-software"

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
# May the person using this machine choose its wifi?
#
# "yes" puts an Internet button in the band at the bottom of every
# screen, which lists the wifi in range and joins one. It is also what
# installs the rule letting the desktop change the network at all: with
# "no", nothing on the machine can, and the network is whatever the
# image or the cable says it is.
#
# On a personal computer this is obviously yes -- it is her house and
# her router, and the alternative is a machine that cannot get online
# without somebody who knows what a terminal is. On a school laptop or
# a machine on a counter it is the owner's decision and not the user's.
# Kiosk builds never get the button regardless.
allow_network_change="yes"
allow_tty="yes"           # Ctrl-Alt-F2 to a console
auto_login="no"
default_user="auros"

# ── Fleet ──────────────────────────────────────────────────────────
enrollment_url=""         # optional MDM / fleet check-in endpoint
update_channel="stable"
telemetry="off"           # off | anonymous — never on by default
