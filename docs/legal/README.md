# Legal documents: DRAFTS ONLY

> **DRAFT: must be reviewed by a lawyer before use.**
> Nothing in this folder is legal advice, and none of it is in force.
> These drafts were written by the project's engineering effort to give a
> lawyer something concrete to start from. They are not published on the
> website or shown in AurOS, and must not be until a qualified lawyer in
> the owner's country has reviewed and approved them.

| File | What it is for | Where it would appear once approved |
|---|---|---|
| [TERMS.md](TERMS.md) | Terms of use for the website, the installer and AurOS | website footer; linked from the installer's first page |
| [PRIVACY.md](PRIVACY.md) | Privacy policy: what AurOS and its website do and don't collect, and which outside services an AurOS computer talks to | website footer; AurOS help pages |
| [DISCLAIMER.md](DISCLAIMER.md) | Plain disclaimer: free software, no warranty, back up first, not affiliated with Microsoft/Canonical/Mozilla | website footer; installer's Ready page (short form) |

## What the owner must do

1. **Take all three to a lawyer** who practises consumer, software and
   data-protection law where you (the publisher) live and where your users
   are. Say who the users are: older, non-technical people, worldwide,
   installing software that changes their computer's drive.
2. **Fill the placeholders.** Every `[OWNER: ...]` marks a fact only you
   can supply: your legal name or company, address, country whose law
   applies, a contact address, an age limit, and whether anything is or
   will be sold.
3. **Ask the lawyer specifically about** (each is flagged in the drafts):
   - whether the warranty disclaimer and liability limits are enforceable
     against consumers in your users' countries (in the EU and UK, for
     example, consumer rights and liability for death, personal injury,
     fraud or gross negligence usually cannot be excluded);
   - **open-source licence duties for the AurOS image.** The image contains
     GPL-licensed programs (the Linux kernel and many others) in binary
     form. Distributing it normally obliges you to provide, or offer in
     writing, the matching source code. Decide how (a source archive per
     release is the usual answer) before the first public release;
   - **trademarks:** "Ubuntu" and Canonical's marks (AurOS ships Ubuntu
     binaries in a modified image; see D8 in
     `docs/handoff/SESSION-2026-10-23.md` and
     `docs/research/other-systems-2026-10.md`), "Firefox", "Windows",
     "BitLocker", "Intel". The drafts use them only to describe
     compatibility and say AurOS is not affiliated; confirm that is enough,
     and whether to write to Canonical as D8 suggests;
   - the **installer's actions** on the user's PC (shrinking the Windows
     drive, pausing BitLocker for one restart, carrying the Wi-Fi password
     across, all with on-screen consent): whether the consent wording on
     screen is adequate, and whether the terms must be accepted before the
     installer changes anything (today it does not ask for acceptance);
   - **data protection:** whether the website's host (GitHub) and the
     download host (GitHub Releases) logging visitors' IP addresses makes
     you a controller of that data, and what the privacy policy must then
     say (GDPR/UK GDPR, California, others);
   - the **safe-harbour promise** in `SECURITY.md`;
   - whether you need an **imprint / legal notice** (required for websites
     run from Germany and some other countries).
4. **If anything is ever sold** (support, a pre-installed computer, a paid
   edition), these drafts are not enough: sales need their own terms
   (price, refunds, cancellation rights, statutory warranty).
5. **Keep the facts true.** The privacy draft lists what AurOS sends where,
   checked against the code on 2026-10-23. Every time a new outside service
   is added (a crash reporter, an update server, analytics), the policy
   must change *before* the release that adds it. Ask an engineer to re-run
   the check described at the end of PRIVACY.md.
6. Only then: publish the approved versions (website footer, help pages),
   and remove the "DRAFT" banners as part of that same change.
