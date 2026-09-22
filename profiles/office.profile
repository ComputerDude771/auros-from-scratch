# ═══════════════════════════════════════════════════════════════════
#  AurOS Profile — Office.
#
#  An organisation's fleet: a charity with forty donated laptops, a
#  practice with six, a library with twelve. Not a kiosk -- the people
#  using these machines do real work on them and need to be able to
#  change things -- and not the personal desktop either, because
#  somebody is responsible for all of them at once and has to be able
#  to say what is on them.
#
#  THE LINE THIS PROFILE DRAWS, and it is a judgement rather than a
#  fact: an administrator may decide what is INSTALLED and how updates
#  arrive; a user may decide how her own machine LOOKS and behaves.
#  Taking the second away is what makes managed fleets miserable, it
#  buys the administrator almost nothing, and the moment a build takes
#  it away somebody starts asking for a personal laptop instead.
# ═══════════════════════════════════════════════════════════════════
inherit="desktop"

profile_id="office"
profile_name="AurOS for Organisations"
profile_description="A managed fleet: the same desktop, with the software set and the updates decided centrally."

os_codename="Quarry"
theme="slate"
shell_archetype="rail"

# ── what an organisation actually needs on the machine ─────────────
packages_apps="libreoffice-writer libreoffice-calc libreoffice-impress
               libreoffice-gtk4
               thunderbird
               evince
               simple-scan
               remmina remmina-plugin-rdp
               keepassxc"

packages_extra="cups cups-client cups-filters system-config-printer
                avahi-daemon
                sssd sssd-tools libnss-sss libpam-sss
                openssh-client
                ca-certificates"

# ── what the administrator decides ─────────────────────────────────
#
# Installing software is the one thing held back, and not because users
# are not to be trusted: it is because forty machines with forty
# different sets of software is forty machines nobody can support, and
# the person who has to support them is usually the one who volunteered.
allow_user_install="no"

# EVERYTHING ELSE SHE KEEPS. The wallpaper, the theme, the font size,
# the keyboard layout, the clock format, whether the dock is on the left
# -- none of it costs the administrator anything and all of it is the
# difference between a computer somebody uses and a computer somebody is
# issued.
allow_settings_change="yes"
allow_theme_change="yes"
allow_network_change="yes"

# A console is not a security boundary on a machine somebody has
# physical access to, and pretending otherwise mostly stops the one
# person who could have fixed something. It stays.
allow_tty="yes"

kiosk_mode="no"
auto_login="no"
default_user=""           # empty = the person who installs it says

# ── fleet ──────────────────────────────────────────────────────────
#
# Both of these are blank in the file as shipped and are filled in by
# whoever rebuilds it for their organisation, which is the whole point
# of a profile. A URL committed here would be a URL forty machines
# check in to by accident.
enrollment_url=""
update_channel="managed"

# Off, and it stays off even in a managed build. An administrator who
# wants inventory has enrollment_url; turning on telemetry for a fleet
# is deciding on forty people's behalf that their machines report what
# they do, and that is not a decision a default should make.
telemetry="off"

screen_off_minutes="15"
