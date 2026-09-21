# AurOS Profile — School Kiosk.
# A locked-down build: one browser, no installs, no settings, no shell.
# Demonstrates that the whole lockdown story is data, not code.
inherit="desktop"

profile_id="school-kiosk"
profile_name="AurOS for Schools (Kiosk)"
profile_description="Locked to a single browser. No installs, no settings, no console."

brand_name="Lincoln High Chromebook Replacement"
theme="sandstone"
shell_archetype="locked"

# The browser comes from the same mechanism the desktop profile uses --
# see browser/browser_source/browser_fallback there. Naming it in
# packages_apps would install Ubuntu's transitional snap package, and
# this build purges snapd, so the kiosk would boot with nothing to run.
packages_apps=""
packages_extra="cups-client"

kiosk_mode="yes"
# Matched against the desktop file's name, the window class it reports
# or its command, so the list covers the browser whichever source the
# build was able to reach. A kiosk that allows only a browser it did not
# manage to install is a kiosk that shows an empty screen.
allowed_apps="firefox firefox-esr epiphany-browser org.gnome.Epiphany falkon"
allow_user_install="no"
allow_settings_change="no"
allow_theme_change="no"
allow_tty="no"
auto_login="yes"
default_user="student"

enrollment_url="https://mdm.lincoln.example/enroll"
update_channel="managed"
