# What stands between this repository and a public download

Everything here is something a stranger's first download needs and this
repository does not yet have. Each item says what it is, what to do,
roughly what it costs, what it unblocks, and the exact switch in the
build that flips once it is done. Code gaps are at the end, because
there are fewer of them than the rest.

The install itself works end to end on synthetic machines, with and
without a memory stick (`docs/PLAN.md` §6, `tools/installtest.sh`,
`tools/nosticktest.sh`). What that does **not** prove is anything
about a real PC; item 5 is where that gets proved.

---

## The order

```
 1 legal entity ───────┬──► 2 code signing ──┐
                       └──► 6 EULA + insurance├──► public download
 3 image host ────────────────────────────────┤
 4 Firefox on the builder ────────────────────┤
 5 real hardware: dry-run fleet ─► spare-PC installs ─► small beta
 7 the code gaps below ───────────────────────┘
```

1 is on the critical path twice (the certificate is issued to it, and
the EULA is between it and the user). 5 is the longest and can start
today: the dry run writes nothing, and the no-stick test build exists.

---

## 1. A legal entity

**Why.** The code-signing certificate's subject name is what Windows
shows in the "do you want to allow this app" prompt, and it has to be a
verified organisation or person. The same entity is who the end-user
licence is with and who is insured. This product repartitions consumer
disks; somebody will lose data, and "a person's GitHub account" is not
somebody a support request or a claim can go to.

**Do.** Register a company where you live (a US LLC, a UK private
limited company, or the local equivalent). Keep its registered name and
address exactly consistent everywhere: the certificate authority checks
them against public records.

**Costs.** Usually tens to a few hundred US dollars in filing fees and
days to a couple of weeks; varies by country and state. An accountant
or a formation service is optional.

**Unblocks.** 2 and 6.

## 2. Code signing

**Why.** An unsigned installer gets the full-screen "Windows protected
your PC" panel whose only visible button is *Don't run*
(`docs/SIGNING.md` explains why that is fatal for this audience), and
Smart App Control on newer Windows 11 installs blocks it outright.

**Do, in order of preference:**

1. **Azure Artifact Signing** (formerly Trusted Signing), if eligible:
   about **USD 9.99 a month**. Organisations in the US, Canada, the EU,
   the UK and several other countries; individuals **only in the US and
   Canada**. Microsoft holds the key and issues short-lived certificates.
2. Otherwise an **OV** code-signing certificate from a public CA
   (DigiCert, Sectigo, GlobalSign, SSL.com, Certum; resellers are
   cheaper than list), roughly **USD 200–400 a year**, delivered on a
   hardware token or a cloud HSM (required since June 2023). Certum's
   open-source certificate is cheapest for an individual.
3. **Not EV.** Since 2024 EV no longer skips the SmartScreen warning;
   it earns reputation exactly like OV. `docs/SIGNING.md` has the
   comparison and the sources.

**Know in advance.** Even a signed installer shows a SmartScreen
warning until enough people have run it. Plan the first release for a
small, known group, not a launch.

**What flips.** Set the signing variables `build/sign` reads
(`AUROS_SIGN_PKCS11_*` for a token, or `AUROS_SIGN_PKCS12` for a
PKCS#12 bridge; `docs/SIGNING.md` has the lines), then
`AUROS_RELEASE=1 ./build/aurbridge`. A release build **refuses to
finish unsigned**, and `tools/signtest.sh` proves the pipeline end to
end with a throwaway CA. Artifact Signing needs its own signing step
added to `build/sign` (it uses Microsoft's `signtool` with a dlib, not
`osslsigncode`); that is a small change and is not written yet.

## 3. A host for the image

**Why.** The installer downloads the 5.3 GB image (1.84 GB compressed)
and checks it against a hash baked in when it was built. It needs an
address that will still answer in a year.

**Today.** The test build downloads it in 20 pieces from the
`image-desktop` branch of this repository through
`raw.githubusercontent.com`. That works (the pieces, their hashes and
resuming have all been checked from here) but it is a git host, not a
download service: it is fine for a handful of test machines and not
for a public release, and the pieces make the repository 1.8 GB larger
until the branch is deleted.

**Do.** One of:

- **GitHub Releases**: each file must be under 2 GiB, and the
  compressed image is 1.84 GB, so it fits as **one** release asset,
  published as a one-line piece list. Free; the simplest step from
  here.
- **Cloudflare R2**, or Backblaze B2 behind a CDN: no or low egress
  fees, which matters at 1.84 GB a download.
- **SourceForge** or another open-source mirror network.

Whatever it is must answer `Range` requests (206 with a Content-Range),
because a dropped connection is resumed rather than restarted; the
installer refuses a server that ignores it rather than writing the
wrong bytes.

**What flips.** A piece list (`src/aurbridge/phases.h` has the format)
and `AUROS_IMAGE_PIECES=<list> AUROS_IMAGE_SHA256=<sha>
AUROS_IMAGE_BYTES=<bytes> ./build/aurbridge`. For a public build add
`AUROS_RELEASE=1`, which refuses without an image address.

## 4. Firefox on the builder

**Why.** The profiles ask for Firefox and the images ship Epiphany: the
network these images are built on answers 403 for every Mozilla and
Launchpad host, so `build/forge` falls back, and says so, only because
`ALLOW_BROWSER_FALLBACK=1` tells it to.

**Do.** Build on a machine or network that can reach
`packages.mozilla.org` (Mozilla's APT repository, the first of the
three routes `build/forge` tries). Nothing in the code changes.

**What flips.** Rebuild **without** `ALLOW_BROWSER_FALLBACK`;
`/etc/auros/build-warnings` in the image must then be empty of browser
lines, and the manifest in `docs/results/` records it.

## 5. Real hardware

**Why.** Every install so far has been in QEMU, on one firmware and one
virtual disk. `docs/results/README.md` lists what that cannot prove:
firmware variety, a drive that lies about having flushed, a real power
supply. The Windows half (`src/aurbridge/plat_win.c`) has only run
under Wine and against a simulated machine.

**Do, in three stages:**

1. **Dry run on as many machines as possible.** Boot the staging
   environment with `aurstage.dry`: it writes nothing, prints one
   machine-readable line, and says whether that machine could be
   converted. Aim for 20 or more old consumer machines from different
   makers and years (HP, Dell, Lenovo, Acer, Toshiba; 2012–2018;
   Insyde, AMI and Phoenix firmware; SATA, NVMe and Intel RST). Keep
   the lines; they are the hardware matrix.
2. **Full installs on spare machines**, with the no-stick test build
   and then with a stick, including pulling the power at each step the
   way `tools/powercuttest.sh` does in QEMU.
3. **A small beta** with people you can talk to, on machines whose
   files are backed up.

**What flips.** Nothing in the build. What changes is the claim in
`README.md`, which today says "not proven on a real PC".

## 6. The licence and insurance

**Do.** An end-user licence and a plain-words disclaimer, reviewed by a
lawyer where the entity is registered; a support address that somebody
reads; and professional-liability (technology errors and omissions)
cover, asked for through a broker. The consent page in the wizard and
`website/safety.html` are written to match; the licence should say the
same things.

## 7. What is still missing in the code

Found by reading the code against what the product says, and still
true on this commit:

- **Screen readers see nothing in the installer.** The wizard is one
  owner-drawn window with no accessibility tree. It needs a UI
  Automation (or MSAA) provider before any public download.
- **The wizard has no page to choose a memory stick**, so it can only
  install in the no-stick mode. The stick mode works in the engine and
  in every end-to-end test, but a person cannot reach it. R11's typed
  confirmation of the target drive belongs on the same page.
- **"Put Windows back" cannot be started by a person.** The restore
  exists and is tested (`aurstage.restore`, from the stick or from the
  copy on the disk), but no menu entry and no button in AurOS starts
  it. The wizard no longer promises one; it needs a start-up menu
  entry and a settings button.
- **The wizard's choices do not reach the installed system.** Language,
  keyboard, time zone, theme and desktop layout are asked for and then
  dropped: the journal carries only the profile. (Ferry does bring the
  keyboard and time zone across from Windows at first boot.)
- **The staging environment has no screen of its own.** It speaks on
  the console, which is readable but is not the progress display the
  design calls for.

---

## What the test build is, and is not

The no-stick installer built from this commit, with the piece list for
the `image-desktop` branch baked in, is for **spare machines and virtual
machines only**. It is unsigned, it is the first time `plat_win.c` meets
a real PC, it keeps the only copy of the way back on the disk it
changes, and nothing a person can press starts "put Windows back".
Windows stays in the firmware's start-up menu throughout, and saying
no to AurOS's first-boot question makes Windows the default again.
