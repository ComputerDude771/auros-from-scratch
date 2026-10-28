# Unfinished work saved as patches (2026-10-23, usage limit)

When the owner's weekly usage limit stopped every agent, nine streams had
edits that were not yet committed. Pushing half-finished code could break
the build, so each is saved here as a patch instead. Nothing is lost.

To use one: check out its **base** commit's state of the files (or the
branch head, and resolve), then `git apply --3way docs/handoff/wip/<ID>.patch`,
finish it, test, commit. Read `docs/handoff/streams/<ID>.md` first.

| Patch | Stream | Base | What it is (the agent's last words) | Notes |
|---|---|---|---|---|
| `F.patch` | real-Windows e2e | 0a603d9 | `tools/e2e/e2e.py`: press Esc only when Edge's download flyout menu is open (avoids a close/reopen loop) | small, likely ready |
| `K.patch` | help/support | 0b55195 | aurshell Help hooks (`foot.c/h`, `main.c`, `shell.h`) + `modaltest.c`/`targets.c` updates; "both compile clean, building the harnesses" | **touches the same aurshell files as S2** — merge carefully |
| `M.patch` | portability | 1b2b93d | new `tools/portabilitytest.sh` (fails on a hard-coded repo name; simulates another repo) | the hard-coded names themselves were partly replaced in pushed commits; finish `tools/e2e/e2e.py` + `publish.sh` fallbacks |
| `R.patch` | Ferry | ea2b042 | new `src/ferry/copy.c` + `ntfsattr.h`; `ferry-files` hands all folders to one `ferry-copy` run that writes as her | step 2 of R's plan |
| `S1.patch` | OS hardening | ac185ad | new `tools/hardenboottest.sh` (boot check of the hardening on Actions) | controls themselves are pushed |
| `S2.patch` | scam protection | 7056069 | new `src/aurshell/guard.c/h` (fullscreen escape / scam guard) + hooks in `foot.c/h`, `main.c`, `shell.h` — **hooks half-done** | conflicts with K |
| `S3.patch` | compiler hardening | c71aacf | `build/hardening.sh` (flags), edits to `build/forge`, `build/staging`, `src/ferry/install.sh`, new `tools/cflagstest.sh`, `tools/fuzz/font.dict`; was about to build everything with the new flags and run cflagstest | prove on Actions before merging (forge!) |
| `S4.patch` | encrypted disk + PIN | 9da7431 | `rootfs/etc/initramfs-tools/` hook + local-top script, `rootfs/usr/lib/auros/lockctl` | design is in `docs/security/THEFT.md` (pushed) |
| `S5.patch` | update channel | bc98068 | `build/aptchannel` changes + new `tools/updatetest.sh` | owner's key steps are in `docs/OWNER-SETUP.md` §4a (pushed) |

Streams A2 (fast path), B, E, H, P had nothing uncommitted.
