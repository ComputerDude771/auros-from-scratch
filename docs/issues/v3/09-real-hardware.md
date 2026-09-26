# 9. Real hardware: almost nothing is proven

**Severity:** the main remaining risk in the project (the project's
own words).

## What happened on the first real PC (2026-09-26)

The first attempt got **no further than Windows' program loader**
([01](01-sxs-manifest.md)). No code from `plat_win.c` ran. So the first
real-hardware result is still to come, and everything below is still
unproven.

## Never run on a real machine

- **`src/aurbridge/plat_win.c` as a whole:** raw disk handles, volume
  dismount, the BitLocker state query, firmware variables, and reading
  `db` and `dbx`. Wine has none of these, so they have only ever run
  against the simulation (`plat_sim.c`).
- **The WinHTTP download.** `plat_fetch` has two implementations. Only
  the simulation's hand-written HTTP is tested (`tools/exetest.sh`).
  WinHTTP has never downloaded a byte in a test.
- **Firmware other than OVMF.** Insyde, AMI and Phoenix firmware from
  2012–2018, which is exactly the hardware this project is for.
  "Firmware variety cannot be synthesised" (`docs/results/README.md`).
- **Real disks:** a drive that lies about flushing, Intel RST/RAID,
  NVMe identifiers as Windows reports them (handled in code, but only
  tested with a faked serial), 4Kn drives on real controllers.
- **A real power supply:** a failing battery cell, a laptop battery that
  cuts out under load. The design's answer is the refusal on battery
  power. That refusal has never read a real battery.
- **The staging environment under Secure Boot on real firmware.** It's
  proven only on Microsoft-keys OVMF (`nosticktest`, `loadertest`).
  `docs/handoff/STATE.md` also asks for proof that raw writes, efivarfs
  writes and module loading work under kernel lockdown. The development
  branch reports they do in QEMU.
- **Antivirus running during the install** ([02](02-antivirus-block.md)).

## What to do (from `docs/RELEASE.md` §5)

1. **Dry-run fleet first.** Boot the staging environment with
   `aurstage.dry` on 20+ old machines (HP, Dell, Lenovo, Acer, Toshiba;
   2012–2018; SATA, NVMe, RST). It writes nothing and prints one line
   per machine. Keep the lines.
2. **Before that, cheaper:** run the console `aurbridge.exe` (it has no
   manifest, so it isn't affected by [01](01-sxs-manifest.md)) as
   administrator on the test PC. It only reads, and it's the first time
   `plat_win.c` meets real hardware.
3. Full installs on spare machines, including pulling the power the way
   `tools/powercuttest.sh` does in QEMU.
4. A small beta with people you can talk to, whose files are backed up.
