#!/bin/sh
# plainwords.sh — does this product talk to her in words she uses?
#
# docs/EASY.md rule 2: "Every user-facing word is one she uses." A word
# she does not use is a word that stops her, and the person this product
# is for stops rather than experiments.
#
# That rule is easy to agree with and easy to forget at four in the
# morning, so this checks it. It reads the strings the product actually
# puts on a screen and fails if any of them contains a word from the
# list in docs/EASY.md.
#
# WHAT IT LOOKS AT, and what it deliberately does not:
#
#   shell_text*(...)        every string the shell paints
#   shells/*.shell          the sentences shown when choosing an
#                           archetype at install time
#   profiles/*.profile      brand_name and profile_description
#   src/aurbridge/wizard.c  the Windows-side wizard's own words
#
# It does NOT look at fprintf(stderr, ...). Developer output is allowed
# to say "compositor" and "stride" and "DRM master", because the person
# reading it is a person who can act on it. Mixing the two lists is how
# a rule like this becomes unenforceable and gets ignored.
#
# It is a grep, not a compiler. It will miss a string assembled at
# runtime, and it cannot tell a comment from code in every case. A miss
# is a bug in this file, not a licence.
#
#   sh tools/plainwords.sh
#
# Exit 0 if every user-facing string is plain, 1 otherwise.
set -u
cd "$(dirname "$0")/.."

# The list is docs/EASY.md's, verbatim. Adding a word here is free.
# Removing one requires changing docs/EASY.md and saying why, because
# the list is the rule and this file is only its enforcement.
JARGON='ssid|wpa|wep|authenticate|authentication|credential|credentials
|interface|adapter|daemon|unmount|partition|repositor|dependenc
|runtime|sandbox|sudo|binary|executable|directory|kernel|compositor
|wayland|xorg|flatpak|initialise|initialize|parameter|validate
|allocate|dhcp|dns|gateway|subnet|protocol|encryption|cipher
|filesystem|bootloader|firmware|chroot|systemd|dbus|xdg'

# Collapsed to one alternation, newlines stripped, so the list above can
# be read by a human.
PAT=$(printf '%s' "$JARGON" | tr -d '\n')

fail=0
checked=0

report() {
    printf '  %-42s %s\n' "$1" "$2"
    printf '      %s\n' "$3"
    fail=$((fail + 1))
}

# ── 1. Strings the shell paints ─────────────────────────────────────
#
# Not "literals on a line that calls shell_text". The first version of
# this did exactly that, reported ok, and could not fail: almost every
# call in this codebase wraps, so the string sits on the NEXT line from
# the function name and was never looked at. A check that cannot fail is
# worse than no check, because it is also a claim.
#
# So: every string literal in the shell, filtered down to the ones that
# look like PROSE -- something with a space in it that is not a path, a
# format string, a theme key or a comment. Lines that write to stderr
# are developer output and are skipped.
echo "strings the shell paints"
for f in src/aurshell/*.c src/aurshell/layouts/*.c; do
    [ -f "$f" ] || continue
    grep -on '"[^"]*"' "$f" 2>/dev/null | while IFS= read -r m; do
        n=${m%%:*}
        lit=${m#*:}
        # Developer output, and comment bodies.
        src=$(sed -n "${n}p" "$f")
        case "$src" in
            *stderr*|*perror*) continue ;;
        esac
        case "$(printf '%s' "$src" | sed 's/^[[:space:]]*//')" in
            '*'*|'/*'*|'//'*) continue ;;
        esac
        body=$(printf '%s' "$lit" | sed 's/^"//; s/"$//')
        # Prose: has a space, has letters, is not a path or a format
        # string or a key=value.
        case "$body" in
            */*|*%*|*=*|'') continue ;;
            # A newline inside the literal means a multi-line block --
            # a --help text or a report. shell_text() paints one line,
            # so nothing it draws contains one. This is also how the
            # developer --help block is recognised: the fputs(..., stderr)
            # that gives it away is a dozen lines below the literal.
            *' '*) ;;
            *) continue ;;
        esac
        # A literal containing a newline escape is a multi-line block --
        # a --help text or a report -- and shell_text() paints one line,
        # so nothing it draws contains one. This is also how a developer
        # --help block is recognised: the fputs(..., stderr) that gives
        # it away sits a dozen lines below the literal, where a
        # same-line check cannot see it.
        printf '%s' "$body" | grep -qF '\n' && continue
        printf '%s' "$body" | grep -q '[A-Za-z][A-Za-z][A-Za-z]' || continue
        hit=$(printf '%s' "$body" | tr 'A-Z' 'a-z' | grep -oE "$PAT" | head -1)
        [ -n "$hit" ] && printf 'HIT\t%s:%s\t%s\t%s\n' "$f" "$n" "$hit" "$body"
    done
done > /tmp/plainwords.shell 2>/dev/null
while IFS="$(printf '\t')" read -r _ loc word lit; do
    report "$loc" "$word" "$lit"
done < /tmp/plainwords.shell
checked=$((checked + 1))
[ -s /tmp/plainwords.shell ] || echo "  ok    nothing the shell paints uses a word she would not"

# ── 2. The sentences shown when choosing an archetype ───────────────
#
# These are read by someone deciding what kind of desktop they want,
# during the install, which is the single most nervous moment in the
# product.
echo
echo "the words that describe each archetype"
found=0
for f in shells/*.shell; do
    [ -f "$f" ] || continue
    for key in shell_name shell_tagline shell_plain shell_best_for \
               shell_tradeoff shell_familiar; do
        val=$(sed -n "s/^$key=\"\(.*\)\"$/\1/p" "$f" 2>/dev/null)
        # Multi-line values: take everything between the quotes.
        [ -n "$val" ] || val=$(awk -v k="$key" '
            $0 ~ "^" k "=\"" { inq=1; sub("^" k "=\"", ""); }
            inq { print; if (/"[[:space:]]*$/) exit }
        ' "$f" 2>/dev/null | tr '\n' ' ')
        [ -n "$val" ] || continue
        hit=$(printf '%s' "$val" | tr 'A-Z' 'a-z' | grep -oE "$PAT" | head -1)
        if [ -n "$hit" ]; then
            report "$f ($key)" "$hit" "$val"
            found=1
        fi
    done
done
[ "$found" -eq 0 ] && echo "  ok    every archetype describes itself plainly"
checked=$((checked + 1))

# ── 3. What the build calls itself ──────────────────────────────────
echo
echo "how a build describes itself"
found=0
for f in profiles/*.profile; do
    [ -f "$f" ] || continue
    for key in brand_name profile_name profile_description; do
        val=$(sed -n "s/^$key=\"\(.*\)\"$/\1/p" "$f" 2>/dev/null)
        [ -n "$val" ] || continue
        hit=$(printf '%s' "$val" | tr 'A-Z' 'a-z' | grep -oE "$PAT" | head -1)
        if [ -n "$hit" ]; then report "$f ($key)" "$hit" "$val"; found=1; fi
    done
done
[ "$found" -eq 0 ] && echo "  ok    every build describes itself plainly"
checked=$((checked + 1))

# ── 4. The Windows-side wizard ──────────────────────────────────────
#
# The highest-stakes words in the product: she reads these while
# deciding whether to let us change her computer.
echo
echo "the words in the installer"
found=0
if [ -f src/aurbridge/wizard.c ]; then
    # stub_log() is developer output for phases that are not built yet
    # -- every one of its strings starts "WOULD". Excluded for the same
    # reason fprintf(stderr) is: the person reading it can act on it.
    grep -n 'L"[^"]*"' src/aurbridge/wizard.c 2>/dev/null \
      | grep -v 'stub_log(' | while IFS= read -r line; do
        n=${line%%:*}
        printf '%s\n' "$line" | grep -o 'L"[^"]*"' | while IFS= read -r lit; do
            hit=$(printf '%s' "$lit" | tr 'A-Z' 'a-z' | grep -oE "$PAT" | head -1)
            [ -n "$hit" ] && printf 'HIT\t%s:%s\t%s\t%s\n' "src/aurbridge/wizard.c" "$n" "$hit" "$lit"
        done
    done > /tmp/plainwords.wiz 2>/dev/null
    while IFS="$(printf '\t')" read -r _ loc word lit; do
        report "$loc" "$word" "$lit"
    done < /tmp/plainwords.wiz
    [ -s /tmp/plainwords.wiz ] || found=0
    [ -s /tmp/plainwords.wiz ] && found=1
fi
[ "$found" -eq 0 ] && echo "  ok    the installer speaks plainly"
checked=$((checked + 1))

rm -f /tmp/plainwords.shell /tmp/plainwords.wiz

echo
if [ "$fail" -gt 0 ]; then
    if [ "$fail" -eq 1 ]; then echo "1 user-facing string uses a word she does not."
    else echo "$fail user-facing strings use words she does not."; fi
    echo "See docs/EASY.md rule 2. The fix is usually a different sentence"
    echo "about what happens next, not a simpler synonym for the same idea."
    exit 1
fi
echo "every user-facing string is in words she uses ($checked places checked)"
exit 0
