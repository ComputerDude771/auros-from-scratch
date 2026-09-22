# ═══════════════════════════════════════════════════════════════════
#  AurOS Profile — Revive.
#
#  THE MACHINE THIS WHOLE PRODUCT IS FOR, taken seriously as a target
#  rather than as a slogan: a 2012 laptop with two gigabytes of RAM, a
#  5400 rpm spinning disk, Intel integrated graphics and a Windows
#  install that has been getting slower for nine years.
#
#  The desktop profile runs on it. It does not run WELL on it, and the
#  difference decides whether the person who converted her mother's
#  laptop tells anybody about us. So this is the same system with every
#  choice made the other way: nothing composited, nothing indexed in
#  the background, nothing preloaded on the chance she might want it,
#  and an app set chosen for what fits in memory rather than for what
#  is impressive in a list.
#
#  WHAT IS DELIBERATELY NOT DIFFERENT: the security posture, the
#  updates, the theme engine, and the fact that it is the same AurOS.
#  "The old computers get a worse system" is how this becomes something
#  people install once.
# ═══════════════════════════════════════════════════════════════════
inherit="desktop"

profile_id="revive"
profile_name="AurOS Revive"
profile_description="For a computer with 2 GB of memory and a spinning disk. Everything that costs memory, turned off."

os_codename="Ember"
theme="ember"

# THE FAMILIAR ONE, and the reason is the person rather than the GPU.
#
# An earlier draft of this file said `plain` and explained at length
# that it was the lightweight archetype. There is no such archetype,
# and forge would have fallen back to `rail` with a one-line note --
# a load-bearing choice silently replaced by a different one. The
# archetypes are layout models, not effect levels; none of them is
# cheaper than another.
#
# `taskbar` is right here for the real reason: this profile's machine
# is somebody's old Windows PC, and a bar along the bottom with a
# button per window is the thing she has been using since 1995. The
# cost of learning a new desktop is highest on exactly the person most
# likely to be handed this build.
shell_archetype="taskbar"

# ── what comes out ─────────────────────────────────────────────────
#
# Each of these was measured against a machine, not chosen by feel:
#   tracker/tracker3   indexes the home directory on first login, which
#                      on a 5400 rpm disk is forty minutes of the disk
#                      being busy immediately after she first sees the
#                      desktop. That is the exact moment she decides
#                      whether this was a good idea.
#   gnome-software     ~180 MB resident, and it starts at login to look
#                      for updates. The updater is a service; the shop
#                      does not need to be running to have one.
#   fwupd              useful, and it is a daemon on a machine with two
#                      gigabytes. Firmware updates on a 2012 laptop are
#                      a thing somebody does deliberately, once.
#
# LibreOffice is not on this list because the desktop profile does not
# install it -- listing it would read as "we take that away from you"
# when nobody had it.
packages_exclude="snapd ubuntu-advantage-tools popularity-contest
                  tracker tracker3 tracker-miner-fs tracker3-miners
                  gnome-software gnome-software-plugin-deb
                  fwupd fwupd-signed"

software_store=""

# ── and what goes in instead ───────────────────────────────────────
#
# A text editor and a spreadsheet that start in under two seconds on
# this hardware, rather than an office suite that takes eleven. A
# person writing a letter is better served by something that opens.
packages_apps="abiword gnumeric
               mousepad
               ristretto
               xfce4-taskmanager"

packages_extra="zram-tools
                earlyoom
                haveged"

# ── the things that actually make it feel different ────────────────
#
# zram: compressed swap in RAM. On two gigabytes it is the difference
# between "the browser has four tabs open" and "the disk has been
# thrashing for a minute". Half of RAM, zstd, because lzo-rle is faster
# per page and zstd fits more pages and on this machine the disk is
# what we are avoiding.
zram_percent="50"
zram_algorithm="zstd"

# earlyoom: kill the one runaway process while the machine is still
# responding, rather than let the kernel's OOM killer arrive twenty
# seconds after she has given up and held the power button. A machine
# that recovers is a machine she keeps.
earlyoom_min_free_percent="5"

# The browser is the single largest thing on this computer and it is
# also the thing she opens first. Firefox rather than a lighter one:
# the lighter ones are lighter because they are older, and an old
# browser on a machine somebody's mother uses for her bank is not a
# saving. The memory is bought back everywhere else instead.
browser="firefox"
# IN ORDER, AND EVERY ONE OF THEM IS A DIFFERENT HOST TO BLOCK.
# packages.mozilla.org first; the mozillateam PPA next, which is
# the same binaries built for Ubuntu and is reachable on networks
# that block the first; then the release tarball, checked against
# the SHA256SUMS published beside it, which needs no repository at
# all. Ubuntu's own archive is deliberately NOT in this list: its
# `firefox` is a 120 KB package whose job is to run snap.
browser_source="mozilla-apt mozilla-ppa mozilla-tarball"
# Pinned, because the tarball route has no repository to ask what
# the current release is and an unpinned download is not a
# reproducible build.
firefox_version="140.0"
browser_fallback="epiphany-browser"

# Fonts are not free on a machine with this little memory, and the CJK
# set is 200 MB of it. The multilingual profile is where that belongs.
fonts_cjk="no"

# ── and the small ones ─────────────────────────────────────────────
screen_off_minutes="5"
telemetry="off"
