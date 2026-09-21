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

packages_apps="firefox"
packages_extra="cups-client"

kiosk_mode="yes"
allowed_apps="firefox"
allow_user_install="no"
allow_settings_change="no"
allow_theme_change="no"
allow_tty="no"
auto_login="yes"
default_user="student"

enrollment_url="https://mdm.lincoln.example/enroll"
update_channel="managed"
