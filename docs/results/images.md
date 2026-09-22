# The five images, as built

One run of `build/all` on a builder with 18 GB free. Each image was
made, hashed, compressed, **decompressed again and checked back
against the digest above**, and only then were the 6.4 GB of
intermediates behind it given back — which is the whole reason five
of them fit.

| profile | image | compressed | peak while building | sha-256 of the image |
|---|---|---|---|---|
| `desktop` | 5.3 GB | 1.5 GB | 7.7 GB | `effbe2c687aaaa33049fc014…` |
| `multilingual` | 5.5 GB | 1.6 GB | 8.3 GB | `6d0dd3482e4a679eee4ff3ce…` |
| `office` | 5.7 GB | 1.7 GB | 8.7 GB | `f9c6b18d666aa313d4b830c6…` |
| `revive` | 5.1 GB | 1.5 GB | 7.3 GB | `49ed7a238f72caf5b371ecd4…` |
| `school-kiosk` | 5.2 GB | 1.5 GB | 7.5 GB | `38f72b502521680934869186…` |

The digests are in `docs/results/<profile>.json`, which is the same
file `build/all --list` reads to know what it has already done. A
second run skips a profile only when the manifest matches the
profile's own content hash *and* the artifact on disk still hashes
to what the manifest says.

## What is not in these images

**Firefox.** `packages.mozilla.org` is unreachable from this build
host, so every one of them fell back to `epiphany-browser` from the
Ubuntu archive — which is forge's designed fallback and it worked, and
is also not what these profiles ask for. A builder that can reach
Mozilla produces the same images with Firefox in them. The image says
so itself: `/etc/auros/build-warnings` carries the line
`browser=epiphany-browser (fallback; ...)`.

