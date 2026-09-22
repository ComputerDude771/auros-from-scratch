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

**Firefox.** Every one of these five fell back to `epiphany-browser`
from the Ubuntu archive. That is what forge was told to do when the
preferred source could not be reached, it worked, and it is also not
what these profiles ask for — and nothing failed, so the images looked
finished. The note was one line in a build log.

Three things have changed since, and one has not.

*The network has not.* This builder's egress policy answers `403` at
the gateway for `packages.mozilla.org`, `ppa.launchpadcontent.net`,
`ftp.mozilla.org` and `download.mozilla.org`; only `archive.ubuntu.com`
and `keyserver.ubuntu.com` are reachable. Ubuntu's own `firefox` is a
121 KB transitional package whose job is to run `snap install`, on an
image that purges snapd — so there is no route to Firefox from here at
all, and the fix for that is the environment's network policy, not the
build. See the *Network access* section of
<https://code.claude.com/docs/en/claude-code-on-the-web>.

*`browser_source` is now a list*, tried in order:
`mozilla-apt mozilla-ppa mozilla-tarball`. The PPA is the same binaries
built for Ubuntu and is reachable on networks that block Mozilla's own
repository; the tarball route needs no repository at all and checks the
download against the `SHA256SUMS` Mozilla publishes beside it, with a
pinned version, because an unpinned download is not a reproducible
build. None of the three can be exercised from here.

*A successful `apt-get install` is no longer taken as a browser.*
`apt-get install firefox` on Ubuntu exits 0, prints nothing alarming
and leaves an image with no browser in it. forge now looks for the
program afterwards, and treats a shell script that mentions snap as
what it is.

*And a substitution now fails the build.* `browser_fallback_ok="yes"`
in the profile, or `ALLOW_BROWSER_FALLBACK=1` in the environment, says
the substitution is acceptable for a build somebody is making for
themselves. Without it, an image that shipped Epiphany where the
profile asked for Firefox does not get built at all — which is the
check that was missing when these five were.

