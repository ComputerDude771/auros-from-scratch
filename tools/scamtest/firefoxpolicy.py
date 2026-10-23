#!/usr/bin/env python3
"""firefoxpolicy.py -- is the browser's scam protection actually on?

rootfs/etc/firefox/policies/policies.json is the one file that decides
how Firefox treats her: HTTPS-only, Safe Browsing she cannot click
through, dangerous downloads blocked, uBlock Origin installed, no
notification pop-ups, no "Leave this page?" trap. Firefox applies it
quietly and drops whatever it does not understand just as quietly -- a
misspelt policy, a value from a newer version, or a preference outside
the short list a policy may set is logged to a console nobody reads and
otherwise ignored. So a control that "is in the file" can be a control
that is not on, and nothing anywhere would say so.

This checks the file against Firefox's OWN rules, vendored next to it:

  firefox-159.0-policies-schema.json        current release (Mozilla apt)
  firefox-esr140.17.1-policies-schema.json  the 140 line (tarball route,
                                            firefox_version="140.0")
  firefox-pref-allowlist.txt                Policies.sys.mjs's list of
                                            preferences a policy may set

and then checks each protection by name. `--prove` breaks each
protection in a copy of the file and insists that its check goes red --
a check that cannot fail is not a check.

    python3 -I tools/scamtest/firefoxpolicy.py [--prove] [policies.json]

Exit 0 when every check passes (and, with --prove, every check was seen
to fail on its broken copy).
"""
import copy
import json
import os
import re
import sys
import urllib.parse

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
DEFAULT = os.path.join(ROOT, "rootfs", "etc", "firefox", "policies", "policies.json")
OLDEST = 140          # the oldest Firefox an image can carry (profiles' firefox_version)
AMO = "https://addons.mozilla.org/"
UBLOCK = "uBlock0@raymondhill.net"

# docs/EASY.md rule 2, the same list tools/plainwords.sh enforces, plus
# the browser's own jargon. The blocked-install message is the one
# sentence in this file she will ever read.
JARGON = re.compile(
    r"\b(ssid|wpa|authenticat\w*|credential\w*|interface|adapter|daemon|"
    r"partition|repositor\w*|dependenc\w*|runtime|sandbox|sudo|binary|"
    r"executable|directory|kernel|flatpak|parameter|validat\w*|"
    r"policy|policies|extension\w*|xpi|install_sources|whitelist\w*|"
    r"enterprise|administrator)\b", re.I)


class Dup(Exception):
    pass


def no_dups(pairs):
    seen = {}
    for k, v in pairs:
        if k in seen:
            raise Dup(k)
        seen[k] = v
    return seen


def load_text(text):
    return json.loads(text, object_pairs_hook=no_dups)


def load(path):
    with open(path, encoding="utf-8") as f:
        return load_text(f.read())


# ── Firefox 140's validator, as far as this file needs it ────────────
#
# The 140 schema is Mozilla's own pre-2026 dialect (types URL, origin,
# JSON; enum; properties/patternProperties), not JSON Schema, so it is
# walked here. Stricter than Firefox on one point, deliberately: a key
# the schema does not know is an error, because Firefox ignores it.

def _is_url(v):
    if not isinstance(v, str):
        return False
    u = urllib.parse.urlsplit(v)
    return bool(u.scheme) and (bool(u.netloc) or u.scheme in ("about", "file", "data"))


def _type_ok(t, v, sch, path, errs):
    if t == "boolean":
        return isinstance(v, bool) or v in (0, 1)
    if t == "string":
        return isinstance(v, str)
    if t in ("number", "integer"):
        return isinstance(v, (int, float)) and not isinstance(v, bool)
    if t == "URL":
        return _is_url(v)
    if t == "URLorEmpty":
        return v == "" or _is_url(v)
    if t == "origin":
        return _is_url(v)
    if t == "JSON":
        if isinstance(v, str):
            try:
                json.loads(v)
                return True
            except ValueError:
                return False
        return isinstance(v, (dict, list))
    if t == "array":
        if not isinstance(v, list):
            return False
        sub = []
        for i, item in enumerate(v):
            old_validate(item, sch.get("items", {}), "%s[%d]" % (path, i), sub)
        errs.extend(sub)
        return True
    if t == "object":
        if not isinstance(v, dict):
            return False
        props = sch.get("properties", {})
        pats = sch.get("patternProperties", {})
        for k, item in v.items():
            if k in props:
                old_validate(item, props[k], path + "." + k, errs)
                continue
            hit = [p for p in pats if re.search(p, k)]
            if not hit:
                errs.append("%s.%s: Firefox 140 does not know this" % (path, k))
            for p in hit:
                old_validate(item, pats[p], path + "." + k, errs)
        for r in sch.get("required", []):
            if r not in v:
                errs.append("%s: %s is required" % (path, r))
        return True
    return False


def old_validate(v, sch, path, errs):
    types = sch.get("type")
    if types is None:
        return
    if not isinstance(types, list):
        types = [types]
    for t in types:
        sub = []
        if _type_ok(t, v, sch, path, sub):
            if "enum" in sch and v not in sch["enum"]:
                errs.append("%s: %r is not one of %s" % (path, v, sch["enum"]))
            errs.extend(sub)
            return
    errs.append("%s: %r is not %s" % (path, v, "/".join(types)))


# ── the checks ───────────────────────────────────────────────────────

def pol(doc):
    return doc.get("policies", {}) if isinstance(doc, dict) else {}


def pref(doc, name):
    p = pol(doc).get("Preferences", {}).get(name)
    return p if isinstance(p, dict) else {}


def locked(doc, name, value):
    p = pref(doc, name)
    return p.get("Status") == "locked" and p.get("Value") is value


def check_shape(doc, ctx):
    errs = []
    if set(doc) != {"policies"}:
        errs.append("top level must be exactly {\"policies\": ...}, got %s" % sorted(doc))
    if not isinstance(doc.get("policies"), dict) or not doc.get("policies"):
        errs.append("\"policies\" must be a non-empty object")
    return errs


def check_schema_159(doc, ctx):
    try:
        import jsonschema
    except ImportError:
        return ["python3-jsonschema is not installed (apt install python3-jsonschema)"]
    v = jsonschema.Draft7Validator(ctx["s159"])
    return ["%s: %s" % ("/".join(str(x) for x in e.absolute_path) or "policies", e.message)
            for e in v.iter_errors(pol(doc))]


def check_schema_140(doc, ctx):
    errs = []
    old_validate(pol(doc), ctx["s140"], "policies", errs)
    return errs


def _added(entry):
    v = (entry.get("x-compatibility", {}).get("firefox", {}) or {}).get("version_added")
    if v is True:
        return 0
    if v in (False, None):
        return None
    m = re.match(r"(\d+)", str(v))
    return int(m.group(1)) if m else None


def check_since_140(doc, ctx):
    errs = []
    props = ctx["s159"].get("properties", {})
    for name in pol(doc):
        if name not in props:
            errs.append("%s: not a Firefox 159 policy at all" % name)
            continue
        a = _added(props[name])
        if a is None or a > OLDEST:
            errs.append("%s: only since Firefox %s; images can carry %d" % (name, a, OLDEST))
    return errs


def check_prefs_allowed(doc, ctx):
    errs = []
    prefixes, security, blocked = ctx["allow"]
    for name, val in pol(doc).get("Preferences", {}).items():
        if name in blocked:
            errs.append("%s: Firefox refuses to let a policy set this" % name)
        elif name.startswith("security."):
            if name not in security:
                errs.append("%s: security.* only by exact name, and this is not one" % name)
        elif not any(name.startswith(p) for p in prefixes):
            errs.append("%s: not a preference a policy may set; Firefox drops it" % name)
        if isinstance(val, dict) and val.get("Status") not in ("locked", "default", "user", "clear"):
            errs.append("%s: Status %r" % (name, val.get("Status")))
    return errs


def need(cond, msg):
    return [] if cond else [msg]


def check_https_only(doc, ctx):
    return need(pol(doc).get("HttpsOnlyMode") == "force_enabled",
                "HttpsOnlyMode must be force_enabled (on, and she cannot be talked into turning it off)")


def check_safebrowsing_no_bypass(doc, ctx):
    return need(pol(doc).get("DisableSecurityBypass", {}).get("SafeBrowsing") is True,
                "DisableSecurityBypass.SafeBrowsing must be true: no 'ignore the risk' on a deceptive site")


def check_safebrowsing_on(doc, ctx):
    errs = []
    for n in ("browser.safebrowsing.malware.enabled",
              "browser.safebrowsing.phishing.enabled",
              "browser.safebrowsing.downloads.enabled",
              "browser.safebrowsing.downloads.remote.enabled"):
        if not locked(doc, n, True):
            errs.append("%s must be locked true" % n)
    return errs


def check_downloads_blocked(doc, ctx):
    errs = []
    for n in ("block_dangerous", "block_dangerous_host",
              "block_potentially_unwanted", "block_uncommon"):
        n = "browser.safebrowsing.downloads.remote." + n
        if not locked(doc, n, True):
            errs.append("%s must be locked true" % n)
    return errs


def check_no_telemetry(doc, ctx):
    p = pol(doc)
    return need(p.get("DisableTelemetry") is True and p.get("DisableFirefoxStudies") is True,
                "DisableTelemetry and DisableFirefoxStudies must both be true")


def check_ublock(doc, ctx):
    e = pol(doc).get("ExtensionSettings", {}).get(UBLOCK, {})
    errs = []
    if e.get("installation_mode") != "force_installed":
        errs.append("uBlock Origin must be force_installed (a scammer cannot talk her into switching it off)")
    if not str(e.get("install_url", "")).startswith(AMO):
        errs.append("uBlock Origin must come from %s, got %r" % (AMO, e.get("install_url")))
    return errs


def check_others_amo_only(doc, ctx):
    star = pol(doc).get("ExtensionSettings", {}).get("*", {})
    errs = []
    if star.get("installation_mode") not in ("allowed", "blocked"):
        errs.append("ExtensionSettings['*'].installation_mode must be set")
    src = star.get("install_sources")
    if not src or any(not str(s).startswith(AMO) for s in src):
        errs.append("add-ons may only be offered by %s, got %r" % (AMO, src))
    if "sitepermission" in star.get("allowed_types", ["sitepermission"]):
        errs.append("allowed_types must be listed and must not include sitepermission")
    if not star.get("blocked_install_message"):
        errs.append("a page that is refused must be told why, in her words (blocked_install_message)")
    return errs


def check_plain_words(doc, ctx):
    msg = pol(doc).get("ExtensionSettings", {}).get("*", {}).get("blocked_install_message", "")
    errs = []
    for m in JARGON.finditer(msg):
        errs.append("blocked_install_message says %r (docs/EASY.md rule 2)" % m.group(0))
    if len(msg.split()) > 40:
        errs.append("blocked_install_message is %d words; she will not read past 40" % len(msg.split()))
    return errs


def check_notifications(doc, ctx):
    n = pol(doc).get("Permissions", {}).get("Notifications", {})
    return need(n.get("BlockNewRequests") is True and n.get("Locked") is True,
                "Permissions.Notifications must block new requests, locked (fake virus alerts arrive this way)")


def check_beforeunload(doc, ctx):
    return need(locked(doc, "dom.disable_beforeunload", True),
                "dom.disable_beforeunload must be locked true: no 'Leave this page?' that keeps her on it")


def check_no_crash_restore(doc, ctx):
    return need(locked(doc, "browser.sessionstore.resume_from_crash", False),
                "browser.sessionstore.resume_from_crash must be locked false: a page closed by force must not come back")


def check_devtools(doc, ctx):
    return need(pol(doc).get("DisableDeveloperTools") is True,
                "DisableDeveloperTools must be true (the refund scam edits her bank page with them)")


def check_no_updater(doc, ctx):
    return need(pol(doc).get("DisableAppUpdate") is True,
                "DisableAppUpdate must be true: this file overrides the install's own, and apt updates Firefox")


CHECKS = [
    # name, function, mutation that must turn it red (None: shown by a text case)
    ("one policies object", check_shape,
     lambda d: d.update({"policy": {}})),
    ("valid for Firefox 159 (Mozilla's own schema)", check_schema_159,
     lambda d: d["policies"].update({"HttpsOnlyMode": "always"})),
    ("valid for Firefox 140 (Mozilla's own schema)", check_schema_140,
     lambda d: d["policies"]["UserMessaging"].update({"NoSuchThing": True})),
    ("every policy works on Firefox 140 and later", check_since_140, "newer"),
    ("every preference is one a policy may set", check_prefs_allowed,
     lambda d: d["policies"]["Preferences"].update(
         {"full-screen-api.warning.timeout": {"Value": 9000, "Status": "locked"}})),
    ("HTTPS only, and locked", check_https_only,
     lambda d: d["policies"].update({"HttpsOnlyMode": "enabled"})),
    ("no clicking past a deceptive-site warning", check_safebrowsing_no_bypass,
     lambda d: d["policies"].pop("DisableSecurityBypass")),
    ("Safe Browsing on, and locked", check_safebrowsing_on,
     lambda d: d["policies"]["Preferences"].pop("browser.safebrowsing.phishing.enabled")),
    ("dangerous and unwanted downloads blocked", check_downloads_blocked,
     lambda d: d["policies"]["Preferences"]["browser.safebrowsing.downloads.remote.block_uncommon"]
     .update({"Value": False})),
    ("no telemetry, no studies", check_no_telemetry,
     lambda d: d["policies"].pop("DisableTelemetry")),
    ("uBlock Origin installed from addons.mozilla.org, always on", check_ublock,
     lambda d: d["policies"]["ExtensionSettings"][UBLOCK].update({"installation_mode": "normal_installed"})),
    ("other add-ons only from addons.mozilla.org", check_others_amo_only,
     lambda d: d["policies"]["ExtensionSettings"]["*"]["install_sources"].append("https://*/*")),
    ("the refusal is in her words", check_plain_words,
     lambda d: d["policies"]["ExtensionSettings"]["*"].update(
         {"blocked_install_message": "Extension installation is blocked by enterprise policy."})),
    ("web notifications refused, locked", check_notifications,
     lambda d: d["policies"]["Permissions"].pop("Notifications")),
    ("no 'Leave this page?' trap", check_beforeunload,
     lambda d: d["policies"]["Preferences"].pop("dom.disable_beforeunload")),
    ("a closed page does not come back after a crash", check_no_crash_restore,
     lambda d: d["policies"]["Preferences"]["browser.sessionstore.resume_from_crash"]
     .update({"Value": True})),
    ("developer tools off", check_devtools,
     lambda d: d["policies"].update({"DisableDeveloperTools": False})),
    ("Firefox does not try to update itself", check_no_updater,
     lambda d: d["policies"].pop("DisableAppUpdate")),
]


def context():
    allow = (set(), set(), set())
    with open(os.path.join(HERE, "firefox-pref-allowlist.txt"), encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            kind, name = line.split(None, 1)
            {"prefix": allow[0], "security": allow[1], "blocked": allow[2]}[kind].add(name)
    return {
        "s159": load(os.path.join(HERE, "firefox-159.0-policies-schema.json")),
        "s140": load(os.path.join(HERE, "firefox-esr140.17.1-policies-schema.json")),
        "allow": allow,
    }


def newer_policy(ctx):
    for name, entry in sorted(ctx["s159"]["properties"].items()):
        a = _added(entry)
        if a is not None and a > OLDEST:
            return name, entry
    return None, None


def mutate(doc, how, ctx):
    d = copy.deepcopy(doc)
    if how == "newer":
        name, entry = newer_policy(ctx)
        if not name:
            return None
        d["policies"][name] = entry.get("examples", [True])[0] if entry.get("examples") else True
    else:
        how(d)
    return d


def main(argv):
    prove = "--prove" in argv
    paths = [a for a in argv if not a.startswith("--")]
    path = paths[0] if paths else DEFAULT
    fail = 0
    print("firefox policy: %s" % os.path.relpath(path, ROOT))

    try:
        doc = load(path)
        print("    %-58s ok" % "strict JSON, no key given twice")
    except Dup as e:
        print("    %-58s FAIL\n        %r appears twice; Firefox keeps only the last" % (
            "strict JSON, no key given twice", str(e)))
        return 1
    except ValueError as e:
        print("    %-58s FAIL\n        %s" % ("strict JSON, no key given twice", e))
        return 1

    ctx = context()
    for name, fn, _ in CHECKS:
        errs = fn(doc, ctx)
        print("    %-58s %s" % (name, "FAIL" if errs else "ok"))
        for e in errs[:8]:
            print("        " + e)
        fail += bool(errs)

    if prove:
        print("each check goes red when its protection is broken:")
        red = 0
        try:
            load_text('{"policies": {"DisableTelemetry": true, "DisableTelemetry": false}}')
            print("    %-58s FAIL (a repeated key was accepted)" % "a repeated key")
            fail += 1
        except Dup:
            red += 1
            print("    %-58s red" % "a repeated key")
        for name, fn, how in CHECKS:
            d = mutate(doc, how, ctx)
            if d is None:
                print("    %-58s FAIL (no way to break it was found)" % name)
                fail += 1
                continue
            errs = fn(d, ctx)
            print("    %-58s %s" % (name, "red" if errs else "STILL GREEN"))
            if errs:
                red += 1
            else:
                fail += 1
        print("    %d/%d checks seen to fail on a broken copy" % (red, len(CHECKS) + 1))

    print("firefoxpolicy: %s" % ("FAIL (%d)" % fail if fail else "ok"))
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
