#!/bin/sh
# ═══════════════════════════════════════════════════════════════════
#  choicestest — does what she chose in the installer reach AurOS?
#
#  rootfs/usr/lib/auros/choices.sh runs at first boot and applies the
#  language, keyboard, time zone, look and desktop that AurBridge wrote
#  to \EFI\AurOS\choices.conf. This runs it against a directory standing
#  in for / (AUROS_CHOICES_ROOT), where it writes files and runs nothing,
#  and checks the files a real system reads:
#
#    /etc/default/locale     LANG             (PAM, systemd)
#    /etc/locale.gen         the locale to generate
#    /etc/default/keyboard   XKBLAYOUT        (aurwl's kb_defaults)
#    /etc/localtime          -> zoneinfo
#    /etc/auros/theme        the next first-boot step's `aurora set`
#    /etc/auros/shell/active.shell -> the archetype
#    /etc/auros/installer-choices.conf   what Ferry must not overwrite
#
#  Windows' own identifiers -- a KLID, a time-zone key name -- are
#  mapped with Ferry's tables, the same ones ferry-settings uses. And
#  the file is input from a FAT partition anybody with Windows could
#  have written, so the last cases are hostile ones: every one must be
#  refused, and must change nothing.
#
#    sh tools/choicestest.sh
# ═══════════════════════════════════════════════════════════════════
set -u
cd "$(dirname "$0")/.."
SCRIPT=rootfs/usr/lib/auros/choices.sh

fail=0; checked=0
ok()  { checked=$((checked+1)); printf '    %-62s %s\n' "$1" "ok"; }
bad() { checked=$((checked+1)); fail=$((fail+1)); printf '    %-62s %s\n' "$1" "FAIL"
        shift; for m in "$@"; do printf '      %s\n' "$m"; done; }
check() { # label  command...
    l=$1; shift
    if "$@" >/dev/null 2>&1; then ok "$l"; else bad "$l"; fi
}

TMP=$(mktemp -d "${TMPDIR:-/tmp}/choicestest.XXXXXX")
[ -n "$TMP" ] && [ -d "$TMP" ] || { echo "no scratch dir"; exit 2; }
trap 'rm -rf "$TMP"' EXIT

# A system with what AurOS ships, and nothing more.
mkroot() {
    R=$1
    rm -rf "$R"
    mkdir -p "$R/etc/default" "$R/etc/auros/shell" "$R/usr/share/ferry" \
             "$R/usr/share/auros/themes" "$R/usr/share/auros/shells" \
             "$R/usr/share/i18n" "$R/usr/share/zoneinfo/Europe" \
             "$R/usr/share/zoneinfo/America"
    cp src/ferry/data/klid.tab src/ferry/data/windowsZones.tab "$R/usr/share/ferry/"
    for t in nocturne moss sandstone synthwave ember slate; do
        : > "$R/usr/share/auros/themes/$t.theme"; done
    for s in rail taskbar tiles dock locked workbench; do
        : > "$R/usr/share/auros/shells/$s.shell"; done
    for z in Europe/Madrid Europe/Paris Europe/London America/Los_Angeles \
             America/New_York; do : > "$R/usr/share/zoneinfo/$z"; done
    printf 'en_US.UTF-8 UTF-8\nes_ES.UTF-8 UTF-8\nfr_FR.UTF-8 UTF-8\n' \
        > "$R/usr/share/i18n/SUPPORTED"
    printf 'en_US.UTF-8 UTF-8\n' > "$R/etc/locale.gen"
    printf 'LANG=en_US.UTF-8\n' > "$R/etc/default/locale"
    printf 'XKBMODEL="pc105"\nXKBLAYOUT="us"\nXKBVARIANT=""\nXKBOPTIONS="compose:ralt"\nBACKSPACE="guess"\n' \
        > "$R/etc/default/keyboard"
    printf 'nocturne\n' > "$R/etc/auros/theme"
    ln -sf /usr/share/auros/shells/rail.shell "$R/etc/auros/shell/active.shell"
}
run() { # root  choices-file
    AUROS_CHOICES_ROOT="$1" AUROS_CHOICES_FILE="$2" sh "$SCRIPT" > "$TMP/out.txt" 2>&1
}
kv() { sed -n "s/^$2=//p" "$1" | tail -n 1; }

echo
echo "Does what she chose in the installer reach AurOS?"

echo
echo "  the ordinary case"
R=$TMP/r1; mkroot "$R"
cat > "$TMP/c1.conf" <<'EOF'
# What was chosen in the AurOS installer.
language=es_ES.UTF-8
keyboard=xkb:es
timezone=iana:Europe/Madrid
theme=moss
shell=taskbar
EOF
run "$R" "$TMP/c1.conf"
check "it finishes"                        test $? -eq 0
check "LANG is Spanish"                    grep -qx 'LANG=es_ES.UTF-8' "$R/etc/default/locale"
check "...and the locale will be generated" grep -qx 'es_ES.UTF-8 UTF-8' "$R/etc/locale.gen"
check "the keyboard is Spanish"            grep -qx 'XKBLAYOUT="es"' "$R/etc/default/keyboard"
check "...keeping the model and options"   grep -qx 'XKBOPTIONS="compose:ralt"' "$R/etc/default/keyboard"
check "the clock is in Madrid" \
    test "$(readlink "$R/etc/localtime")" = /usr/share/zoneinfo/Europe/Madrid
check "the look is Moss"                   grep -qx moss "$R/etc/auros/theme"
check "the desktop is the taskbar one" \
    test "$(readlink "$R/etc/auros/shell/active.shell")" = /usr/share/auros/shells/taskbar.shell
C="$R/etc/auros/installer-choices.conf"
check "all five are recorded for Ferry" \
    test "$(grep -c '=' "$C")" -eq 5

echo
echo "  Windows' own identifiers, mapped with Ferry's tables"
R=$TMP/r2; mkroot "$R"
WTZ="Romance Standard Time"
WANT=$(awk -F'\t' -v k="$WTZ" '/^#/||NF<2{next} $1==k{print $2; exit}' src/ferry/data/windowsZones.tab)
WK=$(awk -F'\t' '/^#/||NF<2{next} $1=="0000040C"{print $2; exit}' src/ferry/data/klid.tab)
printf 'keyboard=klid:0000040c\ntimezone=windows:%s\n' "$WTZ" > "$TMP/c2.conf"
run "$R" "$TMP/c2.conf"
[ -n "$WK" ] && check "a KLID becomes its layout ($WK)" \
    grep -qx "XKBLAYOUT=\"$WK\"" "$R/etc/default/keyboard"
[ -n "$WANT" ] && check "a Windows zone becomes its IANA name ($WANT)" \
    test "$(readlink "$R/etc/localtime")" = "/usr/share/zoneinfo/$WANT"
check "only what was given is recorded" \
    test "$(grep -c '=' "$R/etc/auros/installer-choices.conf")" -eq 2
check "...and the language was not touched" grep -qx 'LANG=en_US.UTF-8' "$R/etc/default/locale"

echo
echo "  nothing chosen, or no file at all"
R=$TMP/r3; mkroot "$R"
before=$(cd "$R" && find . -type f -exec md5sum {} \; | sort | md5sum)
AUROS_CHOICES_ROOT="$R" AUROS_CHOICES_FILE="$TMP/does-not-exist" sh "$SCRIPT" >/dev/null 2>&1
check "no file: it still finishes" test $? -eq 0
after=$(cd "$R" && find . -type f -exec md5sum {} \; | sort | md5sum)
check "...and changes nothing" test "$before" = "$after"

echo
echo "  a file somebody else wrote, with things in it that must be refused"
R=$TMP/r4; mkroot "$R"
before=$(cd "$R" && find . \( -type f -o -type l \) -exec sh -c 'printf "%s %s\n" "$1" "$(readlink "$1" || md5sum < "$1")"' _ {} \; | sort | md5sum)
cat > "$TMP/c4.conf" <<'EOF'
language=xx_XX.UTF-8
keyboard=xkb:us";rm -rf /;"
timezone=iana:../../../etc/shadow
theme=../../../etc/passwd
shell=rail; reboot
EOF
run "$R" "$TMP/c4.conf"
check "it finishes"                         test $? -eq 0
after=$(cd "$R" && find . \( -type f -o -type l \) ! -name installer-choices.conf -exec sh -c 'printf "%s %s\n" "$1" "$(readlink "$1" || md5sum < "$1")"' _ {} \; | sort | md5sum)
check "every hostile value refused, nothing else changed" test "$before" = "$after"
check "and nothing recorded as chosen" \
    test "$(grep -c '=' "$R/etc/auros/installer-choices.conf")" -eq 0
check "each refusal is said out loud" \
    test "$(grep -c 'left as it was' "$TMP/out.txt")" -eq 5

echo
echo "  and the script is never sourced"
R=$TMP/r5; mkroot "$R"
printf 'theme=moss\n$(touch %s/pwned)\n`touch %s/pwned2`\n' "$TMP" "$TMP" > "$TMP/c5.conf"
run "$R" "$TMP/c5.conf"
check "shell syntax in the file is text, not commands" \
    test ! -e "$TMP/pwned" -a ! -e "$TMP/pwned2"
check "...and the valid line beside it still applies" grep -qx moss "$R/etc/auros/theme"

echo
if [ "$fail" -gt 0 ]; then echo "$fail of $checked wrong."; exit 1; fi
echo "$checked checks: what she chose in the installer is what AurOS starts with,"
echo "and a file she did not write cannot choose anything."
exit 0
