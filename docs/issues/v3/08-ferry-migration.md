# 8. Ferry (moving files and settings from Windows): gaps

**Source:** `docs/FERRY.md`, "What cannot migrate" and "Known limitations";
`src/ferry/ferry-dpapi`.
**Overall:** Ferry has only been tested against fixtures
(`src/ferry/tests/run.sh`, 43/43), **never a real person's Windows
data**. Its migration breadth is thin.

| # | Gap | Severity | Notes |
|---|-----|----------|-------|
| 8.1 | **Wi-Fi passwords aren't carried over** | high | `ferry-dpapi wifi-psk` is a stub that always exits 75. Networks are imported without their passwords, so the person has to type each one. `FERRY.md` calls this "the single biggest gap" against the spec. A real implementation can be plugged in through `FERRY_DPAPI_HELPER`, but none exists. |
| 8.2 | **Chrome/Edge/Brave passwords and cookies aren't carried over** | high | The spec's "ask for the old Windows password" path isn't built. Ferry reports the limitation and doesn't prompt. Most people use Chrome or Edge, not Firefox. |
| 8.3 | **Firefox transplant has no Firefox to go into** | high | See [07](07-images-and-build.md) §7.1. The images ship Epiphany. |
| 8.4 | Mail (`.pst` → MBOX) isn't imported | medium | `ferry-detect` counts `.pst` and `.ost` files and reports them. There's no conversion stage. |
| 8.5 | No "your Windows programs → Linux equivalents" list | medium | The app inventory and app-equivalence map are described as Forge data for a future stage. |
| 8.6 | Time zone uses the CLDR "001" default | low | Windows' "Central Standard Time" maps to Chicago even for someone in Mexico. It's shown to confirm, not forced. |
| 8.7 | The registry hive reader skips LOG replay and checksums | low | By design, but a setting changed in the last seconds before an unclean shutdown is missed. |
| 8.8 | Things that can't move at all | accepted | Credential Manager, EFS files, locked BitLocker volumes, DRM media, installed programs, drivers, Windows Hello, Outlook `.ost`. They're listed at the end of every run. That's honest, but it's a long list for someone who was told "your files are already there". |
| 8.9 | OneDrive online-only files | accepted, high impact | They live on the server, not the disk. Ferry refuses to copy the stubs, which is correct. The person has to make OneDrive download everything in Windows first, and nothing in the wizard asks them to. |

**For v3:** 8.1 and 8.9 hit the most people. 8.9 needs a wizard step
("make your OneDrive files available offline before you continue")
more than it needs Ferry code.
