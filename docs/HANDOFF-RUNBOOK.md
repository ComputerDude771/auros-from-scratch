# Handoff runbook: building, testing, publishing

The commands this session actually used, in the order they depend on
each other. `HANDOFF.md` at the top of the repository says what the
state is; this says how to reproduce and move it. Everything here ran
as root in a cloud container with QEMU, OVMF, mingw-w64, Wine, mtools,
sgdisk, sbverify and openssl installed. Nothing in `out/` or `work/` is
in git: a fresh checkout has to rebuild it.

---

## Build order

```sh
./build/forge build desktop     # rootfs from Ubuntu 24.04 packages -> work/forge/desktop/
./build/mkimage desktop         # -> out/auros-desktop.img (about 5.3 GB)
./build/staging                 # -> out/auros-staging-vmlinuz, out/auros-staging.img,
                                #    out/auros-staging-{shimx64,grubx64,mmx64}.efi
                                #    (copied out of the forged rootfs; needs forge first)
./build/aurbridge               # -> out/aurbridge.exe (console), out/aurbridge-wizard.exe,
                                #    out/aurbridge-sim (host simulation, never shipped),
                                #    out/aurbridge-baked.h (generated)
```

`build/staging` enforces write gates: only `src/aurstage/wr.c` may open
anything writable, only `src/aurstage/nvram.c` may write EFI variables.
It refuses the build otherwise.

### The installer as published (image pieces baked in)

```sh
AUROS_IMAGE_PIECES=/path/to/v2/pieces.txt \
AUROS_IMAGE_SHA256=c361d06d700f90feb9628fd3b486b07eb23e1991aa751605409b163f6ba5a45a \
AUROS_IMAGE_BYTES=5325717504 \
./build/aurbridge
cp out/aurbridge-wizard.exe AurOS-Installer-test.exe
```

- Without `AUROS_IMAGE_PIECES` the wizard expects the image beside
  itself (a developer build). `pieces.txt` is baked into C, so the build
  refuses any character outside `[A-Za-z0-9 ._:/#,+-]` (parentheses in
  the comment line broke it once).
- The build reads which Microsoft key the shim is signed with
  (`sbverify --list`) and prints `Secure Boot: preflight checks the
  firmware trusts ...`. `AUROS_RELEASE=1` refuses to build if it cannot.
- Check the result: `python3 tools/pe_payload.py list AurOS-Installer-test.exe`
  should list resources 1-5 (kernel, initramfs, shim, grub, mokmanager);
  `strings AurOS-Installer-test.exe | grep -c image-desktop/v2/` should be 1.
- Get `v2/pieces.txt` from the `image-desktop` branch
  (`git show origin/image-desktop:v2/pieces.txt > pieces.txt`) rather
  than checking the whole branch out; it carries gigabytes of pieces.

---

## Tests

Run them one at a time. **Never rebuild anything in `out/` while an
end-to-end test runs**: they read the staging kernel, initramfs and
`aurbridge-sim` from `out/` mid-run. And **never edit a test script in
place while it runs**: `sh` reads the file as it goes. Write a copy
and `mv` it over; the running shell keeps the old file open.

| Command | Checks | Roughly | Needs |
|---|---|---|---|
| `sh tools/installtest.sh` | 37 | 10-15 min | out/ staging + aurbridge-sim |
| `sh tools/nosticktest.sh` | 45 | 35-45 min (four firmware boots under TCG) | the same, plus `OVMF_CODE_4M.ms.fd`/`OVMF_VARS_4M.ms.fd` |
| `sh tools/choicesboottest.sh` | 9 | about 5 min | `out/auros-desktop.img` |
| `sh tools/choicestest.sh` | 21 | seconds | |
| `sh tools/loadertest.sh` | 18 | long | see `tools/README.md` |
| `cd src/ferry && make && sh tests/run.sh` | 43 | seconds | (19 fail if `make` was skipped) |
| `out/aurbridge-sim sbselftest` | Secure Boot db logic | instant | |
| `out/aurbridge-sim gzselftest` | inflater | instant | |
| `WINEDEBUG=-all wine out/aurbridge.exe selftest` | preflight + inflate + sbdb | seconds | |
| `sh tools/manifesttest.sh out/aurbridge-wizard.exe` | 10 | seconds | mingw, python3 |
| `sh tools/putbacktest.sh` | 16 | about 20 min (three desktop boots under TCG) | `out/auros-desktop.img`, staging, `OVMF_*_4M.ms.fd` |
| `sh tools/firstboottest.sh desktop` | cases A, B, C | about 40 min | `out/auros-desktop.img` |

**Real Windows: `.github/workflows/windows.yml`, on every push to a
`claude/**` branch.** Wine is not Windows (docs/handoff/TRAPS.md): the
first real PC refused an installer every test here had passed. The
workflow builds the `.exe` files, starts them on a GitHub Actions
Windows VM, downloads the published installer and starts that, and
downloads the published image with the installer's own WinHTTP code.
Its `PUBLISHED_SHA256` and `IMAGE_*` variables are changed in the same
commit that publishes a new installer or image. Read results with the
GitHub tools (`actions_list`, `get_job_logs`); the repository is public,
so `curl https://api.github.com/repos/ComputerDude771/auros-from-scratch/actions/runs`
also works without a token.

`tools/README.md` lists every other test. Saved transcripts live in
`docs/results/`, with an index in `docs/results/README.md`; after a run,
copy the log there without its trailing `exit=` line.

### The simulated machine (`out/aurbridge-sim`)

```
out/aurbridge-sim MACHINE-DIR PROFILE STICK-SERIAL IMAGE KERNEL INITRD
```

`STICK-SERIAL` `none` means no-stick mode. `MACHINE-DIR` holds:
`disks.txt` (index, path, bytes, sector, serial, model, removable;
tab-separated), `esp/` (stands in for the EFI partition),
`efivars.txt`, `payload/` (`staging-shim`, `staging-grub`,
`staging-mokmgr`), and for Secure Boot `secureboot` (`1`/`0`),
`db.bin`, `dbx.bin`. Environment: `AURBRIDGE_LANGUAGE`, `_KEYBOARD`,
`_TIMEZONE`, `_THEME`, `_SHELL` (the personalize choices);
`AURBRIDGE_STAGING_KARGS` (extra kernel arguments, tests only).
Other commands: `fetch`, `getimage LIST DEST SHA256 BYTES`,
`sbdb DB-FILE`, `sbselftest`, `gzselftest`.

### Firmware variable stores (`tools/efivarstore.py`)

```sh
python3 tools/efivarstore.py VARS.fd list
python3 tools/efivarstore.py VARS.fd get db OUT.bin        # also dbx
python3 tools/efivarstore.py VARS.fd certs db              # subject names
python3 tools/efivarstore.py VARS.fd db-without "Microsoft Corporation UEFI CA 2011"
python3 tools/efivarstore.py VARS.fd plant Boot0009 "AurOS Installer" '\EFI\AurOS\shimx64.efi'
python3 tools/efivarstore.py VARS.fd entry "AurOS"
python3 tools/efivarstore.py --self-test
```

`db-without` on a copy of `/usr/share/OVMF/OVMF_VARS_4M.ms.fd` is how
the Secured-core and 2023-keys-only firmwares in `nosticktest` are made.
(The other chat's branch changes `plant` to replace rather than append;
see `HANDOFF.md` §6.)

### The wizard under Wine

```sh
WINEDEBUG=-all wine out/aurbridge-wizard.exe --shot  "Z:/abs/dir"   # every page as .bmp
WINEDEBUG=-all wine out/aurbridge-wizard.exe --navtest "Z:/abs/dir" # navtest.log, PASS/FAIL lines
```

There is no PIL in the container. To view a `.bmp`, convert it to PNG
with a few lines of Python (`struct` + `zlib`: read the BMP header,
flip rows, write IHDR/IDAT/IEND). Shot `3b`/`3c` is the Secure Boot
card with its picture.

Wine cannot read or write firmware variables, so preflight under Wine
reports Secure Boot as unknown and never exercises the db check. That
path has run only in the simulation.

---

## Publishing the image in pieces

What was done for v2 (the gzip header says `-9`):

```sh
gzip -9 -c out/auros-desktop.img > auros-desktop.img.gz
split -b 94371840 -d -a 3 auros-desktop.img.gz auros-desktop.img.gz.   # 90 MiB, under GitHub's 100 MB
```

`pieces.txt` format (copy `v2/pieces.txt` as the template):

```
# one comment line, no parentheses
base https://raw.githubusercontent.com/ComputerDude771/auros-from-scratch/image-desktop/vN/
piece auros-desktop.img.gz.000 94371840 <sha256 of that piece>
...
```

Commit into a new directory `vN/` on `image-desktop` (keep older ones so
installers already built against them keep working), add a section to
that branch's `README.md` with the image and gzip SHA-256s, and push.
A 1.8 GB push takes about ten minutes. Then check a piece is reachable
(`curl -sSI .../vN/auros-desktop.img.gz.000`, compare `content-length`)
and let the installer itself prove the list end to end:

```sh
WINEDEBUG=-all wine out/aurbridge.exe getimage "Z:/abs/pieces.txt" "Z:/abs/out.img" SHA256 BYTES
# -> getimage verdict=ok
```

This session worked on `image-desktop` through a git worktree in its
scratch directory. A new container has none; `git fetch origin
image-desktop` followed by `git worktree add ../imgwt image-desktop`
recreates it, at the cost of downloading every piece.

## Publishing the test installer

Replace `AurOS-Installer-test.exe` at the top of `image-desktop`,
update the table in that branch's `README.md` (SHA-256, source commit,
test counts), commit, push, then re-download it and compare:

```sh
curl -sSL -o x.exe https://github.com/ComputerDude771/auros-from-scratch/raw/image-desktop/AurOS-Installer-test.exe
sha256sum x.exe
```

The exe is about 33 MB, over the 30 MiB limit for sending a file into
the chat, which is why it is published on the branch instead.

---

## Space and time

- The container's disk allowance is small (about 11 GB was free
  mid-session). The image is 5.3 GB, its gzip 1.8 GB, the pieces
  another 1.8 GB. Delete `.gz` and piece copies once pushed, and the
  image a `getimage` test wrote.
- `build/all` builds every profile one at a time and frees each one's
  intermediates as it goes (see its header) when space is short.
