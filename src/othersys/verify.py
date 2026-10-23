#!/usr/bin/python3
"""verify — is this the ISO the publisher signed?

    verify.py SYSTEM.json SHA256SUMS SHA256SUMS.gpg [ISO]

1. SHA256SUMS carries a good signature from the key pinned in the system
   manifest (gpgv against the one vendored key, and the signer's
   fingerprint must be the pinned one: a good signature from any other
   key in a keyring is not enough).
2. The line for the manifest's ISO name in that file says the hash the
   manifest pins (a newer point release changes it on purpose, and that
   is a change to review, not to follow).
3. With ISO: the file hashes to it, and is the size the manifest says.

The Windows side needs the same three steps natively (an OpenPGP RSA
verifier for one detached signature; there is none in the tree yet).
"""
import hashlib
import json
import os
import subprocess
import sys


def check_sums(system, sums, sig, keyring):
    r = subprocess.run(['gpgv', '--status-fd', '1', '--keyring', keyring, sig, sums],
                       capture_output=True, text=True)
    fprs = [l.split()[2] for l in r.stdout.splitlines() if l.startswith('[GNUPG:] VALIDSIG ')]
    if r.returncode != 0 or not fprs:
        return ['SHA256SUMS has no good signature (%s)' % (r.stderr.strip().splitlines() or ['?'])[-1]]
    if system['signing_fingerprint'] not in fprs:
        return ['SHA256SUMS is signed by %s, not the pinned key %s' % (fprs, system['signing_fingerprint'])]
    want = None
    for line in open(sums):
        f = line.split()
        if len(f) == 2 and f[1].lstrip('*') == system['iso_name']:
            want = f[0].lower()
    if want is None:
        return ['SHA256SUMS does not list %s' % system['iso_name']]
    if want != system['iso_sha256']:
        return ['SHA256SUMS says %s for %s; the manifest pins %s' % (want, system['iso_name'], system['iso_sha256'])]
    return []


def check_iso(system, iso):
    if os.path.getsize(iso) != system['iso_bytes']:
        return ['%s is %d bytes, not %d' % (iso, os.path.getsize(iso), system['iso_bytes'])]
    h = hashlib.sha256()
    with open(iso, 'rb') as f:
        for b in iter(lambda: f.read(16 << 20), b''):
            h.update(b)
    if h.hexdigest() != system['iso_sha256']:
        return ['%s hashes to %s, not %s' % (iso, h.hexdigest(), system['iso_sha256'])]
    return []


def main():
    sysp, sums, sig = sys.argv[1:4]
    system = json.load(open(sysp))
    keyring = os.path.join(os.path.dirname(os.path.abspath(sysp)), '..', system['signing_key'])
    bad = check_sums(system, sums, sig, keyring)
    if not bad and len(sys.argv) > 4:
        bad = check_iso(system, sys.argv[4])
    for b in bad:
        print('verify: REFUSED: ' + b, file=sys.stderr)
    if not bad:
        print('verify: ok: %s signed by %s%s' % (system['iso_name'], system['signing_fingerprint'],
                                                ', ISO hash matches' if len(sys.argv) > 4 else ''))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
