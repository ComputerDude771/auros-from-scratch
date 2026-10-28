# Theft: an encrypted disk and a short PIN (owner decision D9)

Status: **design, 2026-10-23**. Owner of this file and of the code it
names: stream S4 (`docs/handoff/STREAMS.md`). Implementation status and
next steps: `docs/handoff/streams/S4.md`.

## What it is for, in one sentence

If her laptop is stolen -- switched off, or asleep with the lid shut --
the thief gets a computer, not her photos, her letters, her bank
statements or her Wi-Fi password.

## What she sees

| When | What she sees | Words (EASY.md rules) |
|---|---|---|
| Install, first start | nothing new | -- |
| After "Yes, keep AurOS" | **Choose a PIN**: a big number pad, 4 to 6 numbers, typed twice | "Choose a PIN. You will type it when the computer starts and when it wakes up. Pick 4 to 6 numbers you will remember." |
| Straight after | **Your rescue code**: 8 groups of 8 letters, shown once, with *Print it* and *I have written it down* | "If you ever forget your PIN, this code opens your computer. Write it down or print it, and keep it at home, away from the computer. Nobody else has a copy -- not us either." |
| Every start | a full-screen AurOS page with the number pad (not a console, not grub) | "Type your PIN to start AurOS." Wrong: "That PIN didn't work. Try again." Many wrong: "Too many tries. Wait a few minutes, or use your rescue code." |
| Waking up, or after the screen went dark | the same number pad, over a blurred desktop | "Type your PIN to carry on." |
| Something about the computer changed (firmware, a security setting) | the rescue-code page | "This computer has changed since you last used it, so it needs your rescue code this once." then "Now type your PIN" -- the next start is back to the PIN |
| A PC with no security chip (no TPM 2.0) | the same PIN pages, plus one honest line at "Choose a PIN" | "This computer has no security chip, so choose 6 numbers. Your PIN keeps out a thief, but not an expert with days to spare." |

Nothing here is time-limited or a gesture (EASY rule 5); the number keys
work whatever the keyboard layout (they are read as physical keys), and
the on-screen pad is pressable.

## The design

### 1. The root is LUKS2, encrypted in place, in the background

The AurOS root partition (`AUROS-ROOT`) becomes LUKS2 **after** the
image is on the disk, by `cryptsetup reencrypt --encrypt
--reduce-device-size 32M` (LUKS2 online encryption with a data shift),
the way Windows' device encryption encrypts a working PC: she uses the
computer while it happens, power cuts are survived (LUKS2 reencryption
keeps a resilience journal), and an interrupted pass resumes at the next
start.

Why after, not while copying: the install has two paths. The legacy one
copies the image in staging; the fast path (D1) has *Windows* write the
image raw into the gap. Writing ciphertext from Windows would mean a
LUKS2 + AES-XTS implementation in the Windows installer. Encrypting in
place afterwards works identically for both paths and for an image
written straight to a disk, and changes nothing in the engine.

Where it starts:

1. **AurOS's own initramfs** (`rootfs/etc/initramfs-tools/`, our hook +
   `local-top` script), at any start where the root is still a plain
   ext4 and `/etc/auros/lock` does not say "off": `e2fsck -p`,
   `resize2fs` to 32 MiB below the partition's end, `reencrypt --encrypt
   --init-only` with a temporary keyslot, activate `auros-root`, boot.
   `auros-lock-encrypt.service` then runs `cryptsetup reencrypt
   --resume-only` at idle I/O priority until the whole partition is
   ciphertext.
2. **Staging, later (stream A2's hook)**: `src/aurstage/luks.c` does the
   same right after `commit_settle` and before `switch_root`, so
   encryption has begun before AurOS's first start. Until then the
   first start is plain and encryption begins at the second.

**Her files wait for it.** Ferry (her Windows files) and the PIN only
come after encryption has begun, so her data is never written to the
disk in plaintext (on an SSD, overwriting in place does not erase the
old cells). The order after "Yes, keep AurOS" is: choose PIN -> rescue
code -> (restart once if encryption has not begun: "AurOS will restart
once to lock the drive") -> Ferry.

The temporary keyslot is an **empty passphrase** (pbkdf2, minimum
cost): the disk opens with no input until she chooses a PIN, exactly
like Windows' "waiting for activation" state. It is wiped the moment
the PIN is enrolled, and the state is visible (`lockctl status`:
`state=open-no-pin`). Honest cost: a thief who takes the PC *before*
she chooses a PIN gets what is on it (the OS and nothing of hers, since
Ferry waits); one who copies the LUKS header in that window could later
use the volume key. Re-keying needs a second full pass and is not done.

Free space is encrypted too (the whole partition is rewritten), which
also makes the old Windows deleted files that sat in the gap unreadable.

### 2. Boot files live on AurOS's own EFI partition

grub cannot unlock LUKS2 with a TPM, and Canonical's signed grub cannot
read argon2 keyslots. So the kernel, the initramfs, `grub.cfg` and
`grubenv` live on `AUROS-BOOT` (the copy of the image's own ESP, 512
MiB, ~10 MB used today), under `/auros/`. This is the same layout as
systemd-boot's `$BOOT=ESP`. Concretely:

- `build/mkimage` puts the boot set on the image's ESP and the ESP's
  stub `grub.cfg` loads `/auros/grub.cfg` from the ESP itself when it
  exists (falls back to the root's `/boot/grub/grub.cfg` otherwise, so
  an old image or a plain root still starts).
- `/etc/kernel/postinst.d/zz-auros-lock` and
  `/etc/initramfs/post-update.d/zz-auros-lock` copy each new kernel and
  initramfs there (current + previous, ~180 MB worst case), and
  `bootchain menu` writes the menu to both places.
- `answer.sh` (Put Windows back) already takes `AUROS_GRUBENV`; it
  points at the ESP's grubenv.

The initramfs on the ESP is **not signed** (no keys of our own: D7).
That is the evil-maid gap; see "What it does not protect against".

### 3. Unlock: TPM 2.0 + PIN, sealed to PCR 7

`systemd-cryptenroll --tpm2-device=auto --tpm2-pcrs=7
--tpm2-with-pin=yes` (noble's systemd 255). The key is sealed inside the
TPM; the PIN is the TPM object's authorisation value, so **every wrong
PIN is counted by the TPM itself** (dictionary-attack lockout), not by
our software. systemd 255 talks to the TPM over an encrypted session,
so sniffing the bus of a discrete TPM does not reveal the key.

Why the PIN carries the weight: PCR 7 records the Secure Boot policy
and *which keys* approved the boot, not *what* booted. Any
Microsoft-and-Canonical-signed system (an Ubuntu live stick) produces
the same PCR 7 and could ask the TPM -- so TPM-only (no PIN) with PCR 7
would be defeated by a stick. With the PIN, the thief must guess it, at
the TPM's pace, on this one machine:

| PIN | TPM lockout as Windows leaves it (32 tries, then 1 per 10 min) | 1 per 2 hours |
|---|---|---|
| 4 numbers | about 35 days on average | about 1.1 years |
| 6 numbers | about 9.5 years on average | about 110 years |

(We cannot change those numbers: Windows owns the TPM's lockout
authorisation. `lockctl status` reads and prints the real ones from the
TPM.) The disk taken out and put in another PC is useless without the
rescue code: the sealed key only exists inside this TPM.

The initramfs prompt is ours: `aurlock-pin` (`src/aurshell/lockpin.c`),
a full-screen KMS painter built like `aurscreen`, reading digit keys
from evdev (layout-independent) and unlocking through libcryptsetup's
token API (`crypt_activate_by_token_pin`, dlopened), so it can tell
*wrong PIN* from *TPM locked out* from *this computer changed* and say
the right sentence for each. Without a display it falls back to the
console prompt.

### 4. PCs without TPM 2.0

Common in the target market (pre-2016 PCs, TPM 1.2, firmware TPM off).
The fallback is honest: the PIN becomes a LUKS2 passphrase (argon2id,
1 GiB, about 2 s per try on her machine), minimum **6** numbers there.
Strength: a casual thief is stopped; an expert copying the disk and
guessing offline on a GPU rig needs hours for 6 numbers (minutes for
4), because nothing counts the tries. The words say so (table above).
Better options later: a longer passphrase for people who want one, and
turning on a firmware TPM that is present but off (many 2014-2017 Intel
PCs have PTT in the firmware menu; Settings could explain it).

### 5. When PCR 7 changes (dbx updates, SBAT, firmware, Secure Boot)

PCR 7 changes when the firmware's `db`/`dbx` changes (a dbx update via
fwupd or Windows Update on a dual-boot PC), when shim raises the SBAT
level, when Secure Boot is switched, or after some firmware updates.

- **Planned** (an AurOS update that will change it -- shim-signed,
  fwupd's dbx plugin): `lockctl reseal-begin` runs first and adds a
  second TPM+PIN keyslot bound to **no** PCR. The next start unlocks
  with that (same PIN, still counted by the TPM), and
  `lockctl reseal-finish` (run at every start) re-enrols to the new
  PCR 7 and wipes the unbound slot. Cost: for one start, a signed live
  stick plus her PIN could also unlock -- which PCR 7 never stopped.
- **Unplanned** (Windows Update applied a dbx on the Windows side, a
  firmware update): the TPM refuses, `aurlock-pin` shows the rescue
  code page, then asks her PIN once more; `lockctl` re-enrols to the
  new PCR 7 from the running system. One rescue-code entry, then normal.

### 6. The rescue code

`systemd-cryptenroll --recovery-key`: 64 letters from a set chosen so
that every keyboard layout types them the same (modhex), in 8 groups.
Shown once, in the "Choose a PIN" flow, with *Print it* (when a printer
is set up) and *I have written it down*. **Stored nowhere on the
computer** -- a code kept on the laptop is a code the thief has -- and
nowhere online (no accounts, no Microsoft: D7). A paper sheet at home is
the design; a memory stick is offered as a second copy with the warning
"keep it away from the computer". Settings has *Make a new rescue code*
(needs the PIN; the old code stops working).

### 7. CPUs without AES-NI: Adiantum

`/proc/cpuinfo` without the `aes` flag (Atom, Celeron N, Pentium
before 2010, some VIA) -> `xchacha12,aes-adiantum-plain64` with 4096-
byte sectors (Google's design for exactly these CPUs; the noble kernel
has the modules). With AES-NI -> `aes-xts-plain64`, 512-bit key, 4096-
byte sectors. Chosen at encryption time and recorded in the header.

### 8. The same PIN on wake

Her account's password becomes the PIN (set by `lockctl` as root when
she chooses it). aurshell's lock screen (`src/aurshell/lockscreen.c`)
covers the screen on resume and when the screen goes dark after idle,
and checks the PIN through PAM (`/etc/pam.d/aurlock`: `pam_faillock`
deny=5 unlock_time=60, then `pam_unix` via `unix_chkpwd`). The PIN is
also what she types for anything that needs her permission (the lead's
rule: a downloaded installer asks for the PIN and warns).

### 9. Privilege boundary

aurshell runs as her and never runs anything as root. As with
`answer.sh`, it writes a request file in its own 0700 runtime directory
and a root path unit runs `rootfs/usr/lib/auros/lockctl`, which accepts
only `set-pin` (when no PIN exists), `change-pin` (old PIN checked by
PAM first), `new-rescue-code` (PIN checked) -- digits only, 4-6 (6 with
no TPM) -- and deletes the request after reading it. The PIN never goes
on a command line (systemd-cryptenroll reads `$NEWPIN`), never to the
journal, never to the EFI partition.

## What it does not protect against (said plainly, here and in Help)

- **Her Windows.** Until Windows is removed, the Windows drive is as
  protected as it was: BitLocker PCs stay encrypted (D11 pauses, never
  decrypts); others keep her Windows files readable to a thief. Ferry
  copies them into the encrypted AurOS; it does not delete them.
- **The two-visit attack (evil maid).** The initramfs on the EFI
  partition is not signed (D7: no keys). Someone who gets the laptop,
  plants a fake PIN screen, gives it back, and steals it after she has
  typed her PIN, wins. Also: PIN watched over her shoulder.
- **A running, unlocked computer**, and memory attacks on a sleeping
  one (cold boot, DMA from a malicious device): the key is in memory
  while the disk is open. The lock screen protects her session, not
  the RAM.
- **Before she chooses a PIN** (section 1).

## Performance budget (2013 laptop, 4 GB)

| Cost | Expected | Measured (KVM, Actions) |
|---|---|---|
| Disk throughput, AES-NI | under 5 % on a SATA SSD | see S4.md |
| Disk throughput, Adiantum, no AES-NI | 300-600 MB/s, above a 2013 SSD | see S4.md |
| Start-up | TPM unseal 0.3-1.5 s plus her typing | see S4.md |
| The one-time background pass | about 5 min (SSD) to 30 min (HDD) for 40 GB, idle priority | see S4.md |
| RAM | argon2 only for the no-TPM path, once at start (freed) | -- |

## How it is proven (`.github/workflows/theft.yml`)

KVM VM on Actions, OVMF Secure Boot with Microsoft keys, swtpm as a
TPM 2.0. Each case fails when the thing it checks is removed:

1. plain root -> first start encrypts in place, data survives, the
   partition becomes LUKS2, background pass completes, second start
   boots from LUKS2;
2. PIN enrolled -> the right PIN unlocks (driven through the PIN
   screen with QEMU key presses), a wrong PIN does not, the TPM's
   failure counter rises;
3. PCR 7 changed (Secure Boot keys changed in the VM's variable store)
   -> the TPM refuses, the rescue code unlocks;
4. the raw partition contains no plaintext marker after the pass;
5. timings: start-up, throughput with and without encryption, AES-XTS
   and Adiantum (`cryptsetup benchmark` plus `fio`/`dd`).
