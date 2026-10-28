#!/bin/sh
# ═══════════════════════════════════════════════════════════════════
#  profiletest — does every version of AurOS build, and start up to
#  its desktop?
#
#  "Every version of our OS a user could want" is five profiles, and
#  until this test only one of them (desktop) was ever built or booted
#  by anything that runs on its own. The other four were last built by
#  hand a month ago, with the browser fallback, before the snap pin --
#  and two of them no longer built at all, in ways nothing reported:
#
#    office   asked for `thunderbird`, which in Ubuntu 24.04 is a
#             120 KB package that installs a snap. forge now pins
#             those to -1, so the install line fails forty minutes in
#             with "no installation candidate".
#    revive   excluded `tracker3`, `tracker3-miners` and
#             `gnome-software-plugin-deb`, none of which exist in
#             noble. apt refuses the whole purge for one unknown name,
#             the build says `|| true`, and NOTHING on the list was
#             removed -- including the things that were there.
#
#  Three modes, cheapest first:
#
#    sh tools/profiletest.sh lint [profile...]
#        Seconds, no root. Every package a profile names exists in its
#        suite, none of them is a snap stub, every name it excludes is
#        a real package (so the purge is not a silent no-op), its theme
#        and its desktop exist, a kiosk names the programs it allows.
#        Reads Ubuntu's own package indexes (downloaded once, cached).
#
#    sudo sh tools/profiletest.sh boot <profile> [evidence-dir]
#        The built image, out/auros-<profile>.img: first what is IN it
#        (read off the disk: the packages, the excludes, the policy,
#        the browser), then started under OVMF in QEMU (KVM when there
#        is one) on a 1280x800 screen, until its desktop is up. A probe
#        unit that is NOT part of AurOS -- it is put into a scratch
#        copy of the image and nothing else -- reports from inside over
#        the serial port: which desktop is running, as whom, in which
#        language, which units failed, how long the start took, how
#        much memory is in use. Then the screen is photographed
#        (PNG, in the evidence directory) and asked whether anything
#        was drawn on it.
#
#    sh tools/profiletest.sh selftest
#        Every check above, shown to FAIL on input made to be wrong: a
#        profile with a snap stub, an unknown package, an unknown
#        exclude, a missing theme; a black screen; a report from a
#        machine whose desktop is not the one the profile asks for.
#
#  Environment:
#    PROFILETEST_PKGS     where the package indexes are cached
#    PROFILETEST_TIMEOUT  seconds to wait for the desktop (default 900)
#    PROFILETEST_MEM      MiB for the VM (default: 2048 for revive --
#                         the machine it is for -- else 4096)
# ═══════════════════════════════════════════════════════════════════
set -u
cd "$(dirname "$0")/.."
ROOT=$(pwd)

fail=0; checked=0
ok()  { checked=$((checked+1)); printf '    %-62s %s\n' "$1" "ok"; }
bad() { checked=$((checked+1)); fail=$((fail+1)); printf '    %-62s %s\n' "$1" "FAIL"
        shift; for m in "$@"; do printf '      %s\n' "$m"; done; }

MODE="${1:-}"; [ $# -gt 0 ] && shift

# ── a profile, read the way forge reads it ──────────────────────────
#
# forge sources the inherit chain parent-first; so does this, in a
# subshell, and prints one key. Not a parser of its own: a second
# reading of the same file is how the two come to disagree.
pval() { # profile key
    (
        set +u
        chain=""; cur="$1"; depth=0
        while [ -n "$cur" ]; do
            f="$ROOT/profiles/$cur.profile"
            [ -f "$f" ] || f="$cur"
            [ -f "$f" ] || exit 3
            chain="$f $chain"
            cur=$(sed -n 's/^[[:space:]]*inherit="\{0,1\}\([A-Za-z0-9_-]*\)"\{0,1\}.*/\1/p' "$f" | head -1)
            depth=$((depth+1)); [ $depth -gt 8 ] && exit 3
        done
        for f in $chain; do . "$f"; done
        eval "printf '%s' \"\${$2:-}\"" | tr -s ' \n\t' ' ' | sed 's/^ //; s/ $//'
    )
}

# ── Ubuntu's own package indexes ────────────────────────────────────
PK="${PROFILETEST_PKGS:-${TMPDIR:-/tmp}/profiletest-pkgs}"
indexes() { # suite mirror components -> $PK/<suite>.idx  ("name version" lines)
    s=$1; m=$2; comps=$3
    out="$PK/$s.idx"
    [ -s "$out" ] && return 0
    mkdir -p "$PK"
    : > "$out.new"
    for pocket in "$s" "$s-updates" "$s-security"; do
        for c in $comps; do
            u="$m/dists/$pocket/$c/binary-amd64/Packages.xz"
            curl -fsSL --max-time 300 "$u" -o "$PK/p.xz" \
                || { echo "  could not fetch $u"; rm -f "$out.new" "$PK/p.xz"; return 1; }
            xz -dc "$PK/p.xz" | awk '/^Package: /{p=$2} /^Version: /{print p, $2}' >> "$out.new"
        done
    done
    rm -f "$PK/p.xz"
    sort -u "$out.new" > "$out"; rm -f "$out.new"
}

lint_one() { # profile-name-or-path [index-file]
    p=$1
    case "$p" in
      */*) name=$(basename "$p" .profile)
           [ -f "$p" ] || { bad "$p exists"; return; } ;;
      *)   name=$p
           [ -f "profiles/$p.profile" ] || { bad "profiles/$p.profile exists"; return; } ;;
    esac
    echo
    echo "  $name"
    id=$(pval "$p" profile_id)
    [ "$id" = "$name" ] && ok "profile_id is the file's name" \
                        || bad "profile_id is the file's name" "profile_id='$id'"
    suite=$(pval "$p" base_suite); mirror=$(pval "$p" base_mirror)
    comps=$(pval "$p" base_components)
    idx="${2:-}"
    if [ -z "$idx" ]; then
        indexes "$suite" "$mirror" "$comps" || { bad "the $suite package indexes can be read"; return; }
        idx="$PK/$suite.idx"
    fi
    th=$(pval "$p" theme); sh_=$(pval "$p" shell_archetype)
    [ -f "themes/$th.theme" ] && ok "theme '$th' exists" \
                              || bad "theme '$th' exists" "no themes/$th.theme"
    [ -f "shells/$sh_.shell" ] && ok "desktop '$sh_' exists" \
        || bad "desktop '$sh_' exists" "no shells/$sh_.shell -- forge would quietly use rail"

    # Every list stage 2 hands to one `apt-get install`.
    names="locales"
    for k in packages_hardware packages_boot packages_base packages_desktop \
             packages_files packages_apps packages_extra; do
        names="$names $(pval "$p" $k)"
    done
    missing=""; stubs=""
    for n in $names; do
        v=$(awk -v n="$n" '$1==n {print $2; exit}' "$idx")
        if [ -z "$v" ]; then missing="$missing $n"
        else case "$v" in *snap*) stubs="$stubs $n" ;; esac
        fi
    done
    [ -z "$missing" ] && ok "every package it installs exists in $suite" \
        || bad "every package it installs exists in $suite" "not in $suite:$missing"
    [ -z "$stubs" ] && ok "none of them is a snap stub (pinned out by forge)" \
        || bad "none of them is a snap stub (pinned out by forge)" \
               "these only install a snap, and forge pins them to -1:$stubs"
    unk=""
    for n in $(pval "$p" packages_exclude); do
        awk -v n="$n" '$1==n {f=1} END {exit !f}' "$idx" || unk="$unk $n"
    done
    [ -z "$unk" ] && ok "every name it excludes is a real package" \
        || bad "every name it excludes is a real package" \
               "unknown to apt:$unk -- one unknown name and apt purges NOTHING"
    case "$(pval "$p" kiosk_mode)" in
      yes)
        [ -n "$(pval "$p" allowed_apps)" ] && ok "a kiosk names the programs it allows" \
            || bad "a kiosk names the programs it allows" "allowed_apps is empty"
        ;;
    esac
    u=$(pval "$p" default_user)
    case "$u" in ''|*[!a-z0-9_-]*) bad "default_user is a plain account name" "'$u'" ;;
                 *) ok "default_user is a plain account name ($u)" ;; esac
    if [ -n "$(pval "$p" zram_percent)" ]; then
        case " $names " in *" zram-tools "*) ok "zram asked for, and zram-tools installed" ;;
            *) bad "zram asked for, and zram-tools installed" ;; esac
    fi
    for l in $(pval "$p" locale) $(pval "$p" extra_locales); do
        case "$l" in
          *_*.UTF-8) : ;;
          *) bad "locale '$l' is a UTF-8 locale name" ;;
        esac
    done
}

# ── a picture of the screen, and whether anything is on it ──────────
#
# QEMU writes PPM; this turns it into a PNG with nothing but zlib, and
# says how much of it is not black and how many colours it has. A
# desktop that never drew is a black (or one-colour) frame, and that is
# the failure the picture is there to catch.
shot_py() { # in.ppm out.png  -> "WxH drawn=<permille> colours=<n>"
    python3 - "$1" "$2" <<'EOPY'
import sys, zlib, struct
src, dst = sys.argv[1], sys.argv[2]
d = open(src, 'rb').read()
# P6 header: magic, width, height, maxval, each separated by whitespace
parts = []; i = 0
while len(parts) < 4:
    while d[i:i+1].isspace(): i += 1
    if d[i:i+1] == b'#':
        while d[i:i+1] not in (b'\n', b''): i += 1
        continue
    j = i
    while not d[j:j+1].isspace(): j += 1
    parts.append(d[i:j]); i = j
i += 1
assert parts[0] == b'P6', 'not a P6 PPM'
w, h = int(parts[1]), int(parts[2])
px = d[i:i + w*h*3]
raw = b''.join(b'\0' + px[y*w*3:(y+1)*w*3] for y in range(h))
def chunk(t, b):
    c = struct.pack('>I', len(b)) + t + b
    return c + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) \
      + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b'')
open(dst, 'wb').write(png)
# Sample every 4th pixel of every 4th row: plenty to tell a drawn
# desktop from a black one, and quick on a 1280x800 frame.
lit = 0; n = 0; cols = set()
for y in range(0, h, 4):
    row = px[y*w*3:(y+1)*w*3]
    for x in range(0, w, 4):
        r, g, b = row[x*3], row[x*3+1], row[x*3+2]
        n += 1
        if r + g + b > 48: lit += 1
        if len(cols) < 4096: cols.add((r >> 3, g >> 3, b >> 3))
print('%dx%d drawn=%d colours=%d' % (w, h, lit * 1000 // max(n, 1), len(cols)))
EOPY
}
# A frame counts as a desktop when at least 5% of it is lit and it has
# more than 8 colours (text, icons, a wallpaper). A firmware's black
# screen, a blank framebuffer or one flat colour do not.
drawn_enough() { # "WxH drawn=N colours=M"
    dn=$(printf '%s' "$1" | sed -n 's/.*drawn=\([0-9]*\).*/\1/p')
    cl=$(printf '%s' "$1" | sed -n 's/.*colours=\([0-9]*\).*/\1/p')
    [ "${dn:-0}" -ge 50 ] && [ "${cl:-0}" -gt 8 ]
}

qmp() { # socket command-json...
    python3 - "$@" <<'EOPY'
import sys, socket, json, time
s = socket.socket(socket.AF_UNIX); s.settimeout(30); s.connect(sys.argv[1])
f = s.makefile('rw')
f.readline()
def cmd(c):
    f.write(json.dumps(c) + '\n'); f.flush()
    while True:
        r = json.loads(f.readline())
        if 'return' in r or 'error' in r: return r
cmd({'execute': 'qmp_capabilities'})
for a in sys.argv[2:]:
    r = cmd(json.loads(a))
    if 'error' in r: print(r['error'].get('desc', r)); sys.exit(1)
EOPY
}

# ── what the probe reported, against what the profile asks for ──────
#
# One function for the real boot and for the selftest, so the test of
# the checks is a test of THESE checks.
judge_report() { # profile report-file
    p=$1; R=$2
    rv() { sed -n "s/^PROFILETEST $1=//p" "$R" | tr -d '\r' | tail -1; }
    grep -q '^PROFILETEST up' "$R" && ok "the desktop came up" \
        || bad "the desktop came up" "the probe never saw /run/auros"
    [ "$(rv aurshell)" = active ] && ok "aurshell is running" \
        || bad "aurshell is running" "aurshell.service: $(rv aurshell)"
    want=$(pval "$p" shell_archetype)
    [ "$(rv shell)" = "/usr/share/auros/shells/$want.shell" ] \
        && ok "it is the $want desktop" \
        || bad "it is the $want desktop" "active.shell -> $(rv shell)"
    want=$(pval "$p" theme)
    [ "$(rv theme)" = "$want" ] && ok "it looks like $want" \
        || bad "it looks like $want" "theme: $(rv theme)"
    want=$(pval "$p" default_user)
    [ "$(rv user)" = "$want" ] && ok "it runs as $want, not root" \
        || bad "it runs as $want, not root" "desktop user: $(rv user)"
    want=$(pval "$p" locale)
    [ "$(rv lang)" = "LANG=$want" ] && ok "its language is $want" \
        || bad "its language is $want" "$(rv lang)"
    have=" $(rv locales) "
    miss=""
    for l in $(pval "$p" locale) $(pval "$p" extra_locales); do
        g=$(printf '%s' "$l" | sed 's/\.UTF-8$/.utf8/')
        case "$have" in *" $g "*) : ;; *) miss="$miss $l" ;; esac
    done
    [ -z "$miss" ] && ok "every language it ships was generated" \
        || bad "every language it ships was generated" "not generated:$miss" "have:$have"
    [ -n "$(rv browser)" ] && ok "a browser is installed ($(rv browser))" \
        || bad "a browser is installed" "no firefox on PATH"
    [ -z "$(rv failed)" ] && ok "no unit failed on the way up" \
        || bad "no unit failed on the way up" "failed: $(rv failed)"
    if [ -n "$(pval "$p" zram_percent)" ]; then
        case "$(rv swap)" in *zram*) ok "compressed swap in memory is on" ;;
            *) bad "compressed swap in memory is on" "swap: $(rv swap)" ;; esac
    fi
    if [ -n "$(pval "$p" earlyoom_min_free_percent)" ]; then
        [ "$(rv earlyoom)" = active ] && ok "earlyoom is watching memory" \
            || bad "earlyoom is watching memory" "earlyoom: $(rv earlyoom)"
    fi
    if [ "$(pval "$p" allow_tty)" = no ]; then
        [ "$(rv tty2)" = masked ] && ok "there is no console on Ctrl-Alt-F2" \
            || bad "there is no console on Ctrl-Alt-F2" "getty@tty2: $(rv tty2)"
    fi
}

# ═══ lint ═══════════════════════════════════════════════════════════
if [ "$MODE" = lint ]; then
    for t in curl xz awk; do command -v $t >/dev/null 2>&1 || { echo "need $t"; exit 2; }; done
    [ $# -gt 0 ] || set -- office revive school-kiosk multilingual desktop
    echo
    echo "Would each profile build? (read against Ubuntu's own package lists)"
    for p in "$@"; do lint_one "$p"; done
    echo
    [ "$fail" -eq 0 ] && { echo "$checked checks: every profile asks only for what exists."; exit 0; }
    echo "$fail of $checked wrong."; exit 1
fi

# ═══ selftest ═══════════════════════════════════════════════════════
if [ "$MODE" = selftest ]; then
    T=$(mktemp -d "${TMPDIR:-/tmp}/profiletest-self.XXXXXX")
    trap 'rm -rf "$T"' EXIT
    echo
    echo "Does each check fail when the thing it checks is wrong?"
    # A fake suite: three real names, one snap stub.
    printf '%s\n' "locales 2.39" "mousepad 0.6" "thunderbird 2:1snap1-0ubuntu3" \
                  "tracker 3.7" "snapd 2.6" > "$T/idx"
    # A profile that inherits nothing, so every list is under control.
    cat > "$T/broken.profile" <<'EOP'
profile_id="not-its-name"
base_suite="noble"
theme="no-such-theme"
shell_archetype="rail"
packages_apps="mousepad thunderbird not-a-package"
packages_exclude="snapd tracker3"
kiosk_mode="yes"
allowed_apps=""
default_user="Bad User"
EOP
    # How many of lint_one's checks refuse this profile.
    refusals() { ( fail=0; checked=0; lint_one "$1" "$2" >/dev/null 2>&1; echo "$fail" ); }
    verdict() { # description  refusals  minimum
        if [ "${2:-0}" -ge "$3" ]; then printf '    %-62s %s\n' "$1" "refused ($2)"
        else printf '    %-62s %s\n' "$1" "ACCEPTED -- the check is dead"; fail=$((fail+1)); fi
        checked=$((checked+1))
    }
    # profile_id, theme, missing package, snap stub, unknown exclude,
    # kiosk without apps, default_user: seven.
    verdict "a profile with seven things wrong" "$(refusals "$T/broken.profile" "$T/idx")" 7

    # And each one alone: the real office profile with ONE thing
    # changed, so it is that check and not a neighbour that fires.
    # The index is the real names the desktop and office profiles use
    # (so this part needs no network), plus Ubuntu's thunderbird stub.
    for p in desktop office; do
        for k in packages_hardware packages_boot packages_base packages_desktop \
                 packages_files packages_apps packages_extra packages_exclude; do
            for n in $(pval $p $k); do echo "$n 1.0"; done
        done
    done | grep -v '^thunderbird ' | sort -u > "$T/real.idx"
    printf '%s\n' "locales 2.39" "thunderbird 2:1snap1-0ubuntu3" >> "$T/real.idx"
    one() { # description sed-expr
        sed -e "$2" -e 's/^profile_id=.*/profile_id="one"/' profiles/office.profile > "$T/one.profile"
        verdict "$1" "$(refusals "$T/one.profile" "$T/real.idx")" 1
    }
    sed 's/^profile_id=.*/profile_id="one"/' profiles/office.profile > "$T/one.profile"
    base=$(refusals "$T/one.profile" "$T/real.idx")
    if [ "$base" = 0 ]; then
        printf '    %-62s %s\n' "the office profile as it is now" "passes"
        one "...with thunderbird (a snap stub) added back" 's/^\(packages_apps="\)/\1thunderbird /'
        one "...asking for a package that does not exist" 's/^\(packages_apps="\)/\1no-such-package /'
        one "...excluding a name apt does not know" 's/^shell_archetype=.*/&\npackages_exclude="snapd tracker3"/'
        one "...in a theme that does not exist" 's/^theme=.*/theme="no-such-theme"/'
        one "...on a desktop that does not exist" 's/^shell_archetype=.*/shell_archetype="plain"/'
    else
        printf '    %-62s %s\n' "the office profile as it is now" "FAILS ($base): fix it first"
        fail=$((fail+1))
    fi
    checked=$((checked+1))

    # The screen: black, one flat colour, and something drawn.
    python3 - "$T" <<'EOPY'
import sys, random
t = sys.argv[1]; w, h = 320, 200
def ppm(name, px): open(f'{t}/{name}.ppm', 'wb').write(b'P6\n%d %d\n255\n' % (w, h) + bytes(px))
ppm('black', [0] * (w*h*3))
ppm('flat', [40, 60, 90] * (w*h))
random.seed(1)
px = []
for y in range(h):
    for x in range(w):
        px += [(x * 7) % 256, (y * 5) % 256, ((x + y) * 3) % 256] if y > 20 else [10, 10, 10]
ppm('drawn', px)
EOPY
    for s in black flat; do
        r=$(shot_py "$T/$s.ppm" "$T/$s.png")
        if drawn_enough "$r"; then
            printf '    %-62s %s\n' "a $s screen" "ACCEPTED as a desktop -- dead check"; fail=$((fail+1))
        else
            printf '    %-62s %s\n' "a $s screen" "refused ($r)"
        fi
        checked=$((checked+1))
    done
    r=$(shot_py "$T/drawn.ppm" "$T/drawn.png")
    if drawn_enough "$r" && head -c 8 "$T/drawn.png" | od -An -c | grep -q 'P   N   G'; then
        printf '    %-62s %s\n' "a drawn screen, written as a real PNG" "accepted ($r)"
    else
        printf '    %-62s %s\n' "a drawn screen, written as a real PNG" "REFUSED ($r)"; fail=$((fail+1))
    fi
    checked=$((checked+1))

    # The probe's report: a good one for school-kiosk passes; the same
    # report from a machine on the wrong desktop, as root, with a
    # failed unit, or a missing language, does not.
    good() {
        cat <<EOR
PROFILETEST up 41
PROFILETEST aurshell=active
PROFILETEST shell=/usr/share/auros/shells/locked.shell
PROFILETEST theme=sandstone
PROFILETEST user=student
PROFILETEST lang=LANG=en_US.UTF-8
PROFILETEST locales=C.utf8 en_US.utf8 POSIX
PROFILETEST browser=/usr/bin/firefox
PROFILETEST failed=
PROFILETEST tty2=masked
PROFILETEST done
EOR
    }
    good > "$T/r.good"
    g=$( fail=0; checked=0; judge_report school-kiosk "$T/r.good" >/dev/null; echo "$fail" )
    if [ "$g" = 0 ]; then
        printf '    %-62s %s\n' "a kiosk that came up as its profile says" "accepted"
    else
        printf '    %-62s %s\n' "a kiosk that came up as its profile says" "REFUSED ($g)"
        fail=$((fail+1)); judge_report school-kiosk "$T/r.good" | grep FAIL -A2
    fi
    checked=$((checked+1))
    rep() { # description sed-expr
        good | sed "$2" > "$T/r.bad"
        b=$( fail=0; checked=0; judge_report school-kiosk "$T/r.bad" >/dev/null; echo "$fail" )
        if [ "$b" -ge 1 ]; then printf '    %-62s %s\n' "$1" "refused"
        else printf '    %-62s %s\n' "$1" "ACCEPTED -- the check is dead"; fail=$((fail+1)); fi
        checked=$((checked+1))
    }
    rep "...the desktop never came up"        '/^PROFILETEST up/d'
    rep "...on the taskbar desktop instead"   's/locked.shell/taskbar.shell/'
    rep "...running as root"                  's/user=student/user=root/'
    rep "...in another theme"                 's/theme=sandstone/theme=nocturne/'
    rep "...with a unit that failed"          's/failed=$/failed=auros-firstboot.service/'
    rep "...with no browser"                  's/browser=.*/browser=/'
    rep "...with a console on Ctrl-Alt-F2"    's/tty2=masked/tty2=enabled/'
    rep "...with aurshell crashed"            's/aurshell=active/aurshell=failed/'
    good | sed 's/locales=.*/locales=C.utf8 POSIX/' > "$T/r.bad"
    b=$( fail=0; checked=0; judge_report school-kiosk "$T/r.bad" >/dev/null; echo "$fail" )
    if [ "$b" -ge 1 ]; then printf '    %-62s %s\n' "...with its language never generated" "refused"
    else printf '    %-62s %s\n' "...with its language never generated" "ACCEPTED -- dead"; fail=$((fail+1)); fi
    checked=$((checked+1))
    echo
    [ "$fail" -eq 0 ] && { echo "$checked checks: every check refuses what it exists to refuse."; exit 0; }
    echo "$fail of $checked wrong."; exit 1
fi

# ═══ boot ═══════════════════════════════════════════════════════════
[ "$MODE" = boot ] || { sed -n '2,52p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
P="${1:-}"; [ -n "$P" ] || { echo "usage: $0 boot <profile> [evidence-dir]"; exit 2; }
EV="${2:-$ROOT/out/profiletest-$P}"
for t in qemu-system-x86_64 sgdisk python3 losetup mount; do
    command -v "$t" >/dev/null 2>&1 || { echo "need $t"; exit 2; }
done
[ "$(id -u)" = 0 ] || { echo "boot needs root (it loop-mounts the image)"; exit 2; }
CODE=/usr/share/OVMF/OVMF_CODE_4M.fd; VARS=/usr/share/OVMF/OVMF_VARS_4M.fd
[ -f "$CODE" ] && [ -f "$VARS" ] || { echo "need OVMF"; exit 2; }
IMG="out/auros-$P.img"
if [ ! -f "$IMG" ] && [ -f "$IMG.zst" ]; then IMG="$IMG.zst"; fi
[ -f "$IMG" ] || { echo "no image for $P: run sudo ./build/all $P --keep=raw"; exit 2; }
TIMEOUT="${PROFILETEST_TIMEOUT:-900}"
if [ -n "${PROFILETEST_MEM:-}" ]; then MEM=$PROFILETEST_MEM
elif [ "$P" = revive ]; then MEM=2048; else MEM=4096; fi

mkdir -p "$EV"
T=$(mktemp -d "${TMPDIR:-/tmp}/profiletest.XXXXXX")
QP=""; LP=""
cleanup() {
    [ -n "$QP" ] && kill -9 "$QP" 2>/dev/null
    mountpoint -q "$T/m" 2>/dev/null && umount "$T/m"
    [ -n "$LP" ] && losetup -d "$LP" 2>/dev/null
    rm -rf "$T"
}
trap cleanup EXIT

echo
echo "Does the $P image start up to its own desktop?"
DISK="$T/disk.img"
case "$IMG" in
  *.zst) zstd -q -d --long=27 --sparse "$IMG" -o "$DISK" || { echo "could not unpack $IMG"; exit 2; } ;;
  *)     cp --sparse=always "$IMG" "$DISK" ;;
esac
RAW_BYTES=$(stat -c %s "$DISK")

set -- $(sgdisk -p "$DISK" 2>/dev/null | awk '$NF=="AUROS-ROOT" {print $1, $2, $3}')
[ -n "${2:-}" ] || { echo "  the image has no AUROS-ROOT partition"; exit 1; }
RS=$2; RE=$3
LP=$(losetup --find --show -o $((RS * 512)) --sizelimit $(((RE - RS + 1) * 512)) "$DISK")
mkdir -p "$T/m"; mount "$LP" "$T/m" || { echo "  cannot mount its root"; exit 1; }
M="$T/m"

# ── what is in it, read off the disk ────────────────────────────────
echo
echo "  what the image holds"
installed() { # name -> 0 when dpkg says installed
    awk -v n="$1" 'BEGIN{RS=""} $0 ~ "(^|\n)Package: "n"\n" && $0 ~ /\nStatus: install ok installed/ {f=1} END{exit !f}' \
        "$M/var/lib/dpkg/status"
}
want=$(pval "$P" packages_apps; printf ' '; pval "$P" packages_extra)
miss=""; for n in $want; do installed "$n" || miss="$miss $n"; done
[ -z "$miss" ] && ok "every program the profile asks for is installed" \
    || bad "every program the profile asks for is installed" "missing:$miss"
there=""; for n in $(pval "$P" packages_exclude); do installed "$n" && there="$there $n"; done
[ -z "$there" ] && ok "nothing it excludes is there" \
    || bad "nothing it excludes is there" "still installed:$there"
grep -q "^profile *= *$P\$" "$M/etc/auros/policy.conf" 2>/dev/null \
    && ok "its policy file is this profile's" \
    || bad "its policy file is this profile's" "$(grep '^profile' "$M/etc/auros/policy.conf" 2>/dev/null)"
for k in kiosk_mode allow_user_install allow_tty; do
    v=$(pval "$P" $k); case "$v" in no|false|0|off|n) v=no ;; '') v=x ;; *) v=yes ;; esac
    [ "$v" = x ] && continue
    [ "$k" = kiosk_mode ] || [ "$v" = yes ] || [ "$v" = no ] || continue
    got=$(sed -n "s/^$k *= *//p" "$M/etc/auros/policy.conf" | tr -d ' ')
    [ "$got" = "$v" ] && ok "policy: $k = $v" || bad "policy: $k = $v" "the image says '$got'"
done
bn=$(pval "$P" brand_name)
grep -qx "NAME=\"$bn\"" "$M/etc/os-release" 2>/dev/null \
    && ok "it calls itself \"$bn\"" \
    || bad "it calls itself \"$bn\"" "$(grep '^NAME=' "$M/etc/os-release" 2>/dev/null)"
bw=$(grep '^browser=' "$M/etc/auros/build-warnings" 2>/dev/null | tail -1)
case "$bw" in
  ''|browser=MISSING|*'(fallback;'*) bad "its browser is the one the profile asks for" "${bw:-no browser= record}" ;;
  *) ok "its browser is the one the profile asks for (${bw#browser=})" ;;
esac
installed snapd && bad "there is no snapd" || ok "there is no snapd"
ROOT_USED_MB=$(df -BM --output=used "$M" | tail -1 | tr -dc 0-9)
{
    echo "profile=$P"
    echo "image_bytes=$RAW_BYTES"
    echo "root_used_mb=$ROOT_USED_MB"
    echo "packages=$(grep -c '^Status: install ok installed' "$M/var/lib/dpkg/status")"
    sed 's/^/build-warning: /' "$M/etc/auros/build-warnings" 2>/dev/null
} > "$EV/$P-image.txt"
note_sz="image $(awk -v b="$RAW_BYTES" 'BEGIN{printf "%.2f GiB", b/1073741824}'), root holds ${ROOT_USED_MB} MB"
echo "    $note_sz"

# ── the probe: NOT PART OF AUROS ────────────────────────────────────
install -d "$M/usr/lib/auros-profiletest"
cat > "$M/usr/lib/auros-profiletest/probe.sh" <<'EOS'
#!/bin/sh
# NOT PART OF AUROS. tools/profiletest.sh puts this into a scratch copy
# of an image, and nothing else ever does. It reports, over the serial
# port, what the running system is, once its desktop is up.
S=/dev/ttyS0
# A newline first: the kernel and systemd write to this console too, and
# a report line must start a line of its own to be found.
say() { printf '\nPROFILETEST %s\n' "$*" > "$S"; }
say "probe started"
i=0
while [ ! -d /run/auros ] && [ "$i" -lt 600 ]; do sleep 1; i=$((i+1)); done
[ -d /run/auros ] || { say "never-up after ${i}s: $(systemctl is-active aurshell.service)"; exit 0; }
say "up $(cut -d. -f1 /proc/uptime)"
# Let the start-up FINISH (systemd-analyze answers only once it has),
# then a little more for the desktop's own first minute, before asking
# what failed and how much memory it took.
i=0; until systemd-analyze time >/dev/null 2>&1 || [ "$i" -ge 180 ]; do sleep 1; i=$((i+1)); done
sleep 30
say "aurshell=$(systemctl is-active aurshell.service)"
say "shell=$(readlink /etc/auros/shell/active.shell)"
say "theme=$(cat /etc/auros/theme 2>/dev/null)"
say "user=$(stat -c %U /run/auros)"
say "lang=$(grep '^LANG=' /etc/default/locale)"
say "locales=$(locale -a 2>/dev/null | tr '\n' ' ')"
say "browser=$(command -v firefox || command -v epiphany-browser || true)"
say "failed=$(systemctl --failed --plain --no-legend 2>/dev/null | awk '{print $1}' | tr '\n' ' ' | sed 's/ $//')"
say "swap=$(swapon --show=NAME,TYPE,SIZE --noheadings 2>/dev/null | tr '\n' ';')"
say "earlyoom=$(systemctl is-active earlyoom.service 2>/dev/null)"
say "tty2=$(systemctl is-enabled getty@tty2.service 2>/dev/null)"
say "boot=$(systemd-analyze time 2>&1 | head -1)"
say "jobs=$(systemctl list-jobs --no-legend 2>/dev/null | awk '{print $2}' | tr '\n' ' ')"
say "mem=$(free -m | awk '/^Mem:/ {print "used " $3 " MiB of " $2}')"
say "rootdf=$(df -BM --output=used,size / | tail -1)"
say "kernel=$(uname -r)"
journalctl -b -p err --no-pager -o cat 2>/dev/null | tail -20 | sed 's/^/PROFILETEST journal: /' > "$S"
say "done"
EOS
chmod 755 "$M/usr/lib/auros-profiletest/probe.sh"
cat > "$M/etc/systemd/system/auros-profiletest.service" <<'EOU'
[Unit]
Description=NOT PART OF AUROS: tools/profiletest.sh's probe
# Type=simple and no ordering: the start-up must not wait for the probe
# (a oneshot wanted by multi-user.target holds the target, and the boot
# never "finishes" while it runs, so systemd-analyze has no time to
# give), and After=multi-user.target on a unit that target wants is an
# ordering cycle systemd breaks by dropping a job.
[Service]
Type=simple
ExecStart=/usr/lib/auros-profiletest/probe.sh
[Install]
WantedBy=multi-user.target
EOU
ln -sf /etc/systemd/system/auros-profiletest.service \
    "$M/etc/systemd/system/multi-user.target.wants/auros-profiletest.service"
sync; umount "$M"; losetup -d "$LP"; LP=""

# ── start it ────────────────────────────────────────────────────────
accel=tcg; [ -w /dev/kvm ] && accel=kvm
cp "$VARS" "$T/vars.fd"
SER="$EV/$P-serial.txt"; : > "$SER"
Q="$T/qmp.sock"
qemu-system-x86_64 -machine q35,accel=$accel -cpu max -m "$MEM" -smp 2 \
    -drive if=pflash,format=raw,unit=0,readonly=on,file="$CODE" \
    -drive if=pflash,format=raw,unit=1,file="$T/vars.fd" \
    -drive file="$DISK",format=raw,if=none,id=d0 \
    -device virtio-blk-pci,drive=d0,bootindex=0 \
    -device VGA,edid=on,xres=1280,yres=800 -display none \
    -device qemu-xhci,id=xhci -device usb-tablet,bus=xhci.0 -device usb-kbd,bus=xhci.0 \
    -netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
    -serial file:"$SER" -qmp unix:"$Q",server=on,wait=off \
    >"$T/qemu.log" 2>&1 &
QP=$!
echo
echo "  started ($accel, ${MEM} MiB, 1280x800); waiting for its desktop"
T0=$(date +%s); up=""
while kill -0 "$QP" 2>/dev/null; do
    el=$(( $(date +%s) - T0 ))
    [ "$el" -ge "$TIMEOUT" ] && break
    grep -aq '^PROFILETEST up' "$SER" && [ -z "$up" ] && up=$el
    grep -aq '^PROFILETEST done\|^PROFILETEST never-up' "$SER" && break
    sleep 2
done
shot() { # name -> sets SHOT_STAT
    rm -f "$T/s.ppm"
    if qmp "$Q" "{\"execute\":\"screendump\",\"arguments\":{\"filename\":\"$T/s.ppm\"}}" >/dev/null 2>&1 &&
       [ -s "$T/s.ppm" ]; then
        SHOT_STAT=$(shot_py "$T/s.ppm" "$EV/$1.png")
    else
        SHOT_STAT="no picture"
    fi
}
if kill -0 "$QP" 2>/dev/null; then
    shot "$P"
    S1=$SHOT_STAT
else
    S1="the machine had stopped"
fi
tr -d '\r' < "$SER" | grep -a '^PROFILETEST' > "$EV/$P-report.txt"
[ -n "$QP" ] && kill -9 "$QP" 2>/dev/null; wait "$QP" 2>/dev/null; QP=""

echo
echo "  what the running system said"
judge_report "$P" "$EV/$P-report.txt"
drawn_enough "$S1" && ok "the screen shows a desktop ($S1)" \
                   || bad "the screen shows a desktop" "$S1"
{
    echo "desktop_up_after_s=${up:-never}"
    echo "screen=$S1"
    echo "accel=$accel mem_mib=$MEM"
    sed -n 's/^PROFILETEST \(boot\|mem\|rootdf\|kernel\|swap\)=/\1=/p' "$EV/$P-report.txt"
} >> "$EV/$P-image.txt"
sed -n 's/^PROFILETEST \(boot\|mem\)=/    \1: /p' "$EV/$P-report.txt"
echo "    desktop up after ${up:-never} s (host clock, from power-on)"
echo "    evidence: $EV/$P.png, $P-report.txt, $P-serial.txt, $P-image.txt"
echo
[ "$fail" -eq 0 ] && { echo "$checked checks: $P builds what it says and starts to its desktop."; exit 0; }
echo "$fail of $checked wrong."
exit 1
