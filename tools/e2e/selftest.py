#!/usr/bin/env python3
"""selftest.py -- the e2e harness's own checks, no VM needed.

1. Each variant's page pattern (VARIANT_EXPECT in e2e.py) matches both
   titles the wizard can show for the stop the variant is made for: the
   preflight's (src/aurbridge/preflight.c) and the fix's (fix_win.c).
   Rewording either one would otherwise make the variant fail on a good
   installer, or a loosened pattern pass on the wrong page; and no
   pattern matches another variant's titles.
2. publish.sh's four ways out (the run's release; at the default branch;
   the newest e2e-* release as r<run>-*; none at all), with a fake gh.

Run from anywhere: python3 tools/e2e/selftest.py
"""
import ast, os, re, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "..", "src", "aurbridge")
STOP = {"smalldisk": "insufficient-space", "bitlocker": "bitlocker-system", "pending": "pending-reboot"}
fails = []


def check(ok, what):
    print(("ok   " if ok else "FAIL ") + what)
    if not ok:
        fails.append(what)


def variant_expect():
    tree = ast.parse(open(os.path.join(HERE, "e2e.py")).read())
    for node in tree.body:
        if isinstance(node, ast.Assign) and getattr(node.targets[0], "id", "") == "VARIANT_EXPECT":
            return ast.literal_eval(node.value)
    raise SystemExit("VARIANT_EXPECT not found in e2e.py")


def titles(stop):
    pf = open(os.path.join(SRC, "preflight.c")).read()
    fx = open(os.path.join(SRC, "fix_win.c")).read()
    out = []
    m = re.search(r'add\(r,[^;]*?"%s"[^;]*?PF_BLOCK,\s*"([^"]+)"' % re.escape(stop), pf, re.S)
    out.append(("preflight", m.group(1) if m else None))
    m = re.search(r'\{\s*"%s",\s*FIX_\w+,\s*L"([^"]+)"' % re.escape(stop), fx)
    out.append(("fix", m.group(1) if m else None))
    return out


def test_expect():
    exp = variant_expect()
    check(set(exp) == set(STOP), f"VARIANT_EXPECT covers {sorted(STOP)}")
    all_titles = {v: titles(STOP[v]) for v in STOP}
    for v, want in exp.items():
        for where, t in all_titles[v]:
            check(t is not None, f"{v}: the {where} title of '{STOP[v]}' is in the source")
            if t:
                check(re.search(want["page"], t, re.I) is not None,
                      f"{v}: /{want['page']}/ matches the {where} title '{t}'")
        for other, ts in all_titles.items():
            for where, t in ts:
                if other != v and t:
                    check(re.search(want["page"], t, re.I) is None,
                          f"{v}: /{want['page']}/ does not match {other}'s {where} title")


FAKE_GH = r'''#!/bin/bash
echo "gh $*" >> "$T/calls"
case "$1 $2" in
  "release view") [ -f "$T/rel-$3" ] && exit 0; exit 1 ;;
  "release create")
     tgt=""; for ((i=1;i<=$#;i++)); do [ "${!i}" = --target ] && { j=$((i+1)); tgt=${!j}; }; done
     case "$MODE" in ok) ;; sha403) [ "$tgt" = main ] || { echo "HTTP 403" >&2; exit 1; } ;;
       *) echo "HTTP 403" >&2; exit 1 ;; esac
     touch "$T/rel-$3" ;;
  "api repos/o/r") echo main ;;
  "release list") [ "$MODE" = none ] || echo e2e-111 ;;
  "release upload") echo "$3 ${*:4}" >> "$T/uploads" ;;
esac
exit 0
'''


def test_publish():
    want = {"ok": "e2e-222 e2e-evidence-base.zip", "sha403": "e2e-222 e2e-evidence-base.zip",
            "all403": "e2e-111 r222-e2e-evidence-base.zip", "none": None}
    for mode, upload in want.items():
        with tempfile.TemporaryDirectory() as t:
            os.makedirs(f"{t}/bin"); os.makedirs(f"{t}/out/flow/shots")
            open(f"{t}/bin/gh", "w").write(FAKE_GH); os.chmod(f"{t}/bin/gh", 0o755)
            open(f"{t}/out/flow/timings.md", "w").write("# t\n\n| a | 1 |\n")
            env = dict(os.environ, T=t, MODE=mode, PATH=f"{t}/bin:" + os.environ["PATH"], GH_TOKEN="x",
                       GH_REPO="o/r", GITHUB_RUN_ID="222", GITHUB_SHA="abc", E2E_VARIANT="base",
                       E2E_PUBLISH_STATE=f"{t}/state", GITHUB_STEP_SUMMARY=f"{t}/summary")
            pub = os.path.join(HERE, "publish.sh")
            a = subprocess.run(["bash", pub, "init"], env=env, capture_output=True, text=True)
            b = subprocess.run(["bash", pub, "final", f"{t}/out"], env=env, capture_output=True, text=True)
            ups = open(f"{t}/uploads").read() if os.path.exists(f"{t}/uploads") else ""
            if upload:
                check(ups.startswith(upload), f"publish ({mode}): the evidence goes to '{upload}' (got '{ups.strip()}')")
            else:
                check(ups == "", f"publish ({mode}): nothing is uploaded")
            warned = "::warning::" in a.stdout + a.stderr
            check(warned == (mode != "ok"), f"publish ({mode}): {'warns' if mode != 'ok' else 'no warning'}")
            check(a.returncode == 0 and b.returncode == 0, f"publish ({mode}): never fails the job")


test_expect()
test_publish()
print(f"\n{len(fails)} failed" if fails else "\nall passed")
sys.exit(1 if fails else 0)
