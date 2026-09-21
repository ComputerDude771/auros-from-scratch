#!/bin/sh
# Does the theme engine actually resolve everything it is handed?
#
# aurora is the reskinning contract: a theme is a file, and every
# template in the tree must come out the other side with no token left
# in it. This existed as an assumption until a booted image showed
# `@accent|lighten:18|0x@` sitting in /etc/auros/shell.conf as a
# literal string -- the substitution regex allowed exactly ONE
# `|operation` per token, and five keys in every generated theme used
# two (derive a colour, then format it). The shell read those as
# colours, got zero, and painted with them. Nothing failed loudly.
#
# Run from the repository root. Exits non-zero on any unresolved token.
set -eu
cd "$(dirname "$0")/.."
AURORA="./src/aurora/aurora"
# A scratch state directory, so the test never touches an installed
# machine's active theme.
STATE=$(mktemp -d)
trap 'rm -rf "$STATE"' EXIT
export AURORA_THEMES=themes AURORA_TEMPLATES=themes/templates
export AURORA_STATE="$STATE" AURORA_CACHE="$STATE/cache"

# The colour algebra is tested against a fixture with fixed values, not
# against a shipped theme. It used to use Nocturne's, so redesigning
# Nocturne's palette failed twelve assertions about arithmetic that had
# not changed -- a test that breaks when the thing it is not testing
# changes is a test people learn to ignore.
mkdir -p "$STATE/themes"
cp themes/*.theme "$STATE/themes/" 2>/dev/null || :
cat > "$STATE/themes/_algebra.theme" <<'FIXTURE'
theme_name="Algebra Fixture"
theme_variant="dark"
bg="#0B0E14"
bg_alt="#10151F"
surface="#161C28"
surface_hi="#1F2735"
overlay="#2B3542"
muted="#55606E"
subtle="#8793A4"
fg="#D4DCEA"
fg_hi="#F3F7FD"
accent="#7DD3C0"
accent_alt="#A78BFA"
accent_warm="#F2B880"
ok="#7DD3C0"
warn="#F2B880"
err="#F2788D"
info="#82AAFF"
FIXTURE
printf '_algebra\n' > "$STATE/theme"

fail=0
checked=0

# 1. The algebra itself, including chains and the error paths.
probe() {
    got=$(printf '%s\n' "$1" | $AURORA render 2>/dev/null)
    checked=$((checked + 1))
    if [ "$got" != "$2" ]; then
        printf '  FAIL  %-34s got "%s", want "%s"\n' "$1" "$got" "$2"
        fail=1
    else
        printf '  ok    %-34s %s\n' "$1" "$got"
    fi
}

echo "colour algebra (against a fixture, not a shipped theme)"
AURORA_THEMES_REAL=themes
export AURORA_THEMES="$STATE/themes"
probe '@accent|0x@'                    '0x7DD3C0'
probe '@accent|hex@'                   '7DD3C0'
probe '@accent|rgb@'                   '125,211,192'
probe '@accent|lighten:18|0x@'         '0x94DBCB'
probe '@accent|darken:20|lighten:10|0x@' '0x74B2A4'
probe '@bg_alt|mix:bg:20|0x@'          '0x0F141D'
probe '@accent|on|0x@'                 '0x0B0E14'
probe '@bg|on|0x@'                     '0xF3F7FD'
probe '@nosuchkey|0x@'                 '@@MISSING:nosuchkey@@'
probe '@accent|nosuchop@'              '@@BADOP:nosuchop@@'
probe 'plain text, no tokens'          'plain text, no tokens'
probe '@accent|0x@ and @bg|0x@'        '0x7DD3C0 and 0x0B0E14'

export AURORA_THEMES="$AURORA_THEMES_REAL"

# 2. Every template against every theme, which is the real contract.
#    `aurora render` uses the ACTIVE theme, so the active theme is
#    switched for each one and restored afterwards. Rendering every
#    template against a single theme would miss a theme that simply
#    does not define a key the templates use.
echo
echo "every template x every theme"
for th in themes/*.theme; do
    n=$(basename "$th" .theme)
    printf '%s\n' "$n" > "$STATE/theme"
    bad_total=0
    for tpl in themes/templates/*.tpl; do
        checked=$((checked + 1))
        out=$(tail -n +2 "$tpl" | $AURORA render 2>/dev/null || true)
        # An unresolved token is either our own error marker or a
        # literal @token| that the regex declined to match.
        bad=$(printf '%s' "$out" | grep -c '@@\|@[a-z_][a-z0-9_]*|' || true)
        if [ "$bad" != "0" ]; then
            printf '  FAIL  %-12s %-22s %s unresolved\n' "$n" "$(basename "$tpl")" "$bad"
            printf '%s' "$out" | grep -n '@@\|@[a-z_][a-z0-9_]*|' | head -3 | sed 's/^/          /'
            fail=1; bad_total=$((bad_total + bad))
        fi
    done
    [ "$bad_total" = 0 ] && printf '  ok    %-12s all templates resolve\n' "$n"
done
printf 'nocturne\n' > "$STATE/theme"

# 3. The target ships mawk, not gawk. They must agree byte for byte --
#    a gawk extension slipping in here (strtonum) cost a silent theming
#    failure on the first real image build. This runs aurora twice with
#    `awk` pointed at each, rather than trusting whichever is default.
echo
echo "mawk and gawk agree"
if command -v mawk >/dev/null 2>&1 && command -v gawk >/dev/null 2>&1; then
    shim=$(mktemp -d)
    render_with() {
        ln -sf "$(command -v "$1")" "$shim/awk"
        PATH="$shim:$PATH" $AURORA render
    }
    a=$(cat themes/templates/*.tpl | render_with mawk 2>/dev/null | md5sum)
    b=$(cat themes/templates/*.tpl | render_with gawk 2>/dev/null | md5sum)
    checked=$((checked + 1))
    rm -rf "$shim"
    if [ "$a" = "$b" ]; then
        echo "  ok    identical output  ($a)"
    else
        echo "  FAIL  mawk $a"
        echo "        gawk $b"
        fail=1
    fi
else
    echo "  skipped: need both mawk and gawk installed"
fi

echo
if [ $fail = 0 ]; then
    echo "$checked checks passed"
else
    echo "FAILED"
fi
exit $fail
