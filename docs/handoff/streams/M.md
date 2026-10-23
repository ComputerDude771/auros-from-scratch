# Stream M — portability: no hard-coded repository

## Goal
The project will move to another GitHub repository. After a move, nothing
but the clone URL and branch (docs/MOVING.md) may need changing. So no file
outside `docs/` may name the repository (`<owner>/<name>`,
`<owner>.github.io/<name>`, `raw.githubusercontent.com/<owner>/...`):
- workflows use `${{ github.repository }}` / `$GITHUB_REPOSITORY`;
- scripts ask `sh tools/repo.sh [repo|owner|name|site|host|releases]`
  (AUROS_REPO, else GITHUB_REPOSITORY, else the `origin` remote);
- the website: the deploy writes the repository into the pages; door.js
  also derives the download address from its own `<owner>.github.io/<name>/`;
- the wizard's website address is baked by build/aurbridge from tools/repo.sh.

## Done and pushed
- `tools/repo.sh` (new): the one place that answers "which repository".
- `.github/workflows/e2e.yml` INSTALLER_URL and `windows.yml` PUBLISHED_URL,
  KNOWN_BAD_URL, IMAGE_PIECES_URL: repo part is `${{ github.repository }}`.

## Still to do (in order)
1. build/aurbridge: write `AUROS_SITE_W` (L"<host>") into out/aurbridge-baked.h;
   src/aurbridge/fix_win.c uses it for ADVANCED.
2. website: replace the address with tokens filled at deploy
   (`tools/site/stamp.sh`), pages.yml stamps then checks the stamped copy;
   door.js derives from location on *.github.io; check.mjs derives
   RELEASE_URL and BASE from tools/repo.sh.
3. tools/e2e/e2e.py PUBLISHED_URL, tools/{e2e,othersys}/publish.sh GH_REPO
   fallback, tools/e2e/README.md examples -> derived / OWNER/REPO.
4. `tools/portabilitytest.sh`: fails if the literal (or the current repo)
   appears outside docs/README.md/HANDOFF.md; simulates AUROS_REPO=someone/other.

## Still tied to the old repository (by nature)
- README.md's clone command and docs/** (the clone URL is what changes).
- HANDOFF.md records the historic published link.
- The pinned published files (image-desktop branch, commit d2735b6) exist only
  in the old repository unless that branch is pushed to the new one too; the
  published installer ef903efd has its image address baked in.

## Verify
`sh tools/portabilitytest.sh` (once it lands); `sh tools/repo.sh site`.
