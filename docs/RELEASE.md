# What stands between this repository and a public download

> **2026-10-23.** Builds, tests and releases now happen on GitHub
> Actions ("How a release is made", next), and AurOS-Setup.exe is
> **unsigned by the owner's decision** (§2). Items 3 and 4 below are
> done by that pipeline.

---

## How a release is made

Three workflows, all on `ubuntu-24.04` runners (pinned: `ubuntu-latest`
moves to 26.04 in November 2026), all triggered by a push to a
`claude/**` branch. `workflow_dispatch` cannot be used: it only works
for workflows on the repository's default branch. **The trigger words
count only in the commit's subject line.**

| To get | Push a commit that | What runs |
|---|---|---|
| A build and every test | changes `build/`, `src/`, `rootfs/`, `profiles/`, `themes/`, `shells/` or the workflows, or says `[build]` in its subject | `build.yml`: build, then `tests.yml` |
| Tests only, no build | says `[test]` in its subject | `tests.yml` against the newest build of the branch whose artifacts have not expired (14 days) |
| A prerelease, built and tested | says `[release]` | `build.yml`, `tests.yml`, then `release.yml` if every test passed |
| A prerelease of a build that already passed | says `[publish]` (newest passing build of the branch) or `[publish RUN-ID]` | `release.yml` only: nothing is rebuilt, so what is published is exactly what was tested |
| The full release (moves `releases/latest/...`, which the website links to) | says `[publish] [latest]` or `[publish RUN-ID] [latest]` | `release.yml`: the prerelease (made, or found and checked to be the same file), its read-back and Windows checks, then `promote`, which requires a passing `e2e.yml` run whose `INSTALLER_SHA256` is this AurOS-Setup.exe |

Every release starts as a prerelease; only `promote` makes one public.
So the order for a release the website serves is: `[release]` (or
`[publish]`) → pin `INSTALLER_URL`/`INSTALLER_SHA256` in `e2e.yml` to
that prerelease's `AurOS-Setup.exe` and let it pass → `[publish RUN-ID]
[latest]`. A flaky test: "Re-run failed jobs" on the build run (or the
GitHub tools' `rerun_failed_jobs`), then `[publish RUN-ID]`.

A commit can be empty (`git commit --allow-empty -m "[publish] ..."`).

**`build.yml`** (`build` job, about an hour): frees disk, installs the
build tools, `build/all desktop --keep=raw` **without**
`ALLOW_BROWSER_FALLBACK` (the runner reaches Mozilla, so Firefox must
arrive or the build stops), `build/staging`, `gzip -9` and `split` into
90 MiB pieces with `pieces.txt` whose `base` is
`https://github.com/ComputerDude771/auros-from-scratch/releases/download/v<os_version>-<sha7>/`,
then `AUROS_RELEASE=1 build/aurbridge` with that list baked in. It then
asks the `.exe` itself what it carries (resources 1-5, the release
address, the shim's Microsoft key, the manifest) and has
`aurbridge-sim getimage` -- the installer's own phase engine and
inflater -- fetch every piece over HTTP with `Range` from a local
server, unpack it and check the image's SHA-256. Artifacts:
`auros-installer` (AurOS-Setup.exe, aurbridge.exe, aurbridge-sim, the
staging files, pieces.txt, the image's hash and size, the manifest,
`/etc/auros/build-warnings` from the image, timings), `auros-pieces`
and `auros-rootfs` (the forged rootfs, for the tests).

**`tests.yml`**: `unit` (aurfirsttest, choicestest, nvramtest,
bridgetest, manifesttest, Ferry, the simulation's selftests,
`aurbridge.exe selftest` under Wine, signtest) and one `e2e` job per
end-to-end test under KVM (installtest, nosticktest, firstboottest,
choicesboottest, putbacktest). The scripts ask QEMU for `accel=tcg`;
the job puts a `qemu-system-x86_64` in `/usr/local/bin` that turns it
into `accel=kvm` (or `$AUROS_QEMU_ACCEL`), so no test script is edited.
Each job uploads its transcript as `transcripts-<test>`.

**`release.yml`**: creates prerelease `v<os_version>-<sha7>` (the tag baked
into that build's installer) with `AurOS-Setup.exe`, the pieces,
`pieces.txt` and `SHA256SUMS`, and notes saying what was tested where;
then reads every file back from its public address through GitHub's
302 and checks a `Range` request is answered 206 from the right byte;
then, on a Windows runner, starts the downloaded `AurOS-Setup.exe`
(`--navtest`) and has `aurbridge.exe getimage` -- WinHTTP, the
installer's own download code -- fetch the image from the release and
check it.

Reading results: `actions_list` / `get_job_logs` with the GitHub tools,
or `https://api.github.com/repos/ComputerDude771/auros-from-scratch/actions/runs`
without a token. Artifact downloads are blocked from the development
container; release assets are not.

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
 1 legal entity ──────────► 6 EULA + insurance┬──► public download
 2 code signing: not done, by decision ───────┤
 3 image host: GitHub Releases, done ─────────┤
 4 Firefox on the builder: Actions, done ─────┤
 5 real hardware: dry-run fleet ─► spare-PC installs ─► small beta
 7 the code gaps below ───────────────────────┘
```

1 is on the critical path for the EULA, which is between it and the
user. 5 is the longest and can start
today: the dry run writes nothing, and the no-stick test build exists.

---

## 1. A legal entity

**Why.** The entity is who the end-user licence is with and who is
insured. (It was also going to be the name on a code-signing
certificate; there will not be one, §2.) This product repartitions consumer
disks; somebody will lose data, and "a person's GitHub account" is not
somebody a support request or a claim can go to.

**Do.** Register a company where you live (a US LLC, a UK private
limited company, or the local equivalent). Keep its registered name and
address exactly consistent everywhere.

**Costs.** Usually tens to a few hundred US dollars in filing fees and
days to a couple of weeks; varies by country and state. An accountant
or a formation service is optional.

**Unblocks.** 6.

## 2. Code signing: not done, by decision

**The owner's decision (2026-10-23): no code signing, no certificates,
no Azure, no Microsoft services.** AurOS-Setup.exe is published
unsigned. `docs/SIGNING.md` says what the person sees and clicks: Edge
"isn't commonly downloaded" → **Keep** → **Keep anyway**; the blue
SmartScreen panel → **More info** → **Run anyway**; on a PC with Smart
App Control on, Smart App Control has to be switched off first.
`AUROS_RELEASE=1` builds an unsigned installer; the release notes say
so in one neutral line.

## 3. A host for the image: GitHub Releases (done by the pipeline)

The image is published as release assets, in 90 MiB gzip pieces listed
in `pieces.txt` (each asset must be under 2 GiB; pieces keep a dropped
connection cheap). The address the installer has baked in is
`https://github.com/ComputerDude771/auros-from-scratch/releases/download/<tag>/<piece>`;
GitHub answers it with a 302 to a short-lived signed storage URL, which
answers `Range` with 206. WinHTTP follows the redirect on every request,
and every request is a fresh one to the original address, so a resume
an hour later gets a fresh signed URL rather than an expired one. The
release workflow checks all of this after every release, the last part
on a real Windows runner. Releases have no bandwidth limit.

The `image-desktop` branch (pieces through `raw.githubusercontent.com`)
is the older route and stays for installers already built against it.

## 4. Firefox on the builder: done by building on Actions

The images built in the development container shipped Epiphany, because
that network answers 403 for Mozilla. `build.yml` builds on a GitHub
runner **without** `ALLOW_BROWSER_FALLBACK`, so `build/forge` must
install Firefox from Mozilla's APT repository or stop. Every build
uploads the image's `/etc/auros/build-warnings` and the release notes
quote it.

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
- **AurOS's own text is English only.** The language chosen in the
  installer now reaches the installed system (below), and sets the
  locale -- dates, numbers, sorting -- and every application that ships
  its own translations. The shell's own words, and applications whose
  translations Ubuntu moves into separate language packs, stay English
  until those packs are installed.
- **The staging environment has no screen of its own.** It speaks on
  the console, which is readable but is not the progress display the
  design calls for.
- **A PC that trusts only Microsoft's 2023 third-party key cannot use
  this installer with Secure Boot on.** Ubuntu's shim is signed with
  the 2011 key; preflight finds such a PC from Windows and says the
  installer is too old for it, before anything changes. What closes it
  is a shim signed with the 2023 key (Ubuntu's, when it ships one);
  `build/aurbridge` reads the key off whatever shim it embeds, so
  nothing else needs to change.

### Found while making the test build, and fixed

Recorded because each of them passed every test that existed, and each
would have stopped the first real install:

- **A Secured-core PC would have installed nothing, and said nothing.**
  Some PCs (some Surface, some Lenovo) ship with Secure Boot trusting
  Microsoft's Windows key only. The firmware refuses shim at the
  restart, spends `BootNext` and starts Windows again. Preflight now
  reads the firmware's `db` and `dbx` from Windows and stops first,
  with the one setting drawn (`docs/AURBRIDGE.md`, "Secure Boot stays
  on"). The old warning, which asked for consent to a Secure Boot PC
  as if something might go wrong, is gone: an ordinary Secure Boot PC
  now simply passes.
- **A PC that dual-boots Ubuntu was refused, and every PC got a file in
  `\EFI\ubuntu`.** The installer wrote grub's configuration where the
  prefix baked into Canonical's grub points, and refused if a real
  Ubuntu's was there. Loaded from `\EFI\AurOS`, that grub reads the
  configuration beside itself first; the installer now writes only
  under `\EFI\AurOS` (R12), and the test installs with a real Ubuntu's
  `grub.cfg` on the same partition and leaves it alone.
- **The documents said the shim was signed with Microsoft's 2011 and
  2023 keys.** It is signed with Canonical's and Microsoft's 2011 key
  (`sbverify --list`); "dualsigned" means those two. See §7.

- **The restart would not have started the installer on a PC with
  Secure Boot on** -- nearly every Windows 10 and 11 PC. The boot entry
  named the staging kernel itself, which Canonical signs and firmware
  does not trust; every end-to-end test handed QEMU the kernel with
  `-kernel`, which skips the question. The entry now starts the image's
  Microsoft-signed shim, which starts Canonical's grub, which starts the
  kernel; `tools/nosticktest.sh` boots it from firmware with Microsoft's
  keys and Secure Boot enforcing, and shows the old entry being refused.
- **An NVMe laptop would have been refused after its restart, about
  its own disk.** The staging environment matched the disk by the
  serial AurBridge read on Windows, exactly, and on NVMe Windows
  reports an identifier like `0025_3886_81B7_8F17.` where Linux reports
  the drive's serial. It now also accepts the serial written
  differently, and failing that the one disk whose partition table has
  the recorded hash (which includes the disk's own GUID); two such
  disks are a refusal. `tools/nosticktest.sh` installs with the two
  serials deliberately different.
- **Nothing chosen on the personalize page reached AurOS.** Language,
  keyboard, time zone and look were words on chips with no values
  behind them, and the language went through as the constant `en`. Each
  chip now carries a value; phase 3 writes them to
  `\EFI\AurOS\choices.conf`; first boot applies them
  (`rootfs/usr/lib/auros/choices.sh`) and records them so Ferry does
  not overwrite them. On the way: no image had the `locales` package, so
  `LANG=en_US.UTF-8` named a locale that did not exist, and Ferry wrote
  the keyboard and language it imported from Windows to files nothing
  reads. `tools/choicestest.sh`, `tools/choicesboottest.sh`.
- **The last button undid the install.** After phase 3 the only button
  said *Close*, and closing the window takes the one-shot start-up
  setting back by design. It says *Restart now* and restarts, keeping
  the setting; closing still takes everything back.
- **The installer never asked for administrator rights**, so a
  double-click landed on a refusal page. It carries a manifest now.
- **The wizard could not install at all in the stick mode** -- it never
  nominated a stick -- so every install from the GUI would have failed
  in phase 2. The test build is no-stick; the stick page is still owed
  (above).
- **"Replace Windows completely" did nothing**: chosen, it installed
  beside a Windows that was still there. It is no longer offered.
- **"Put Windows back" was promised on the consent page** as a menu
  entry that does not exist. The page now says what is true.

---

## What the test build is, and is not

The no-stick installer built from this commit, with the piece list for
the `image-desktop` branch baked in, is for **spare machines and virtual
machines only**. It is unsigned, it is the first time `plat_win.c` meets
a real PC, it keeps the only copy of the way back on the disk it
changes, and nothing a person can press starts "put Windows back".
Windows stays in the firmware's start-up menu throughout, and saying
no to AurOS's first-boot question makes Windows the default again.
