# Problems found on the first real Windows PC

On 2026-09-26 the test installer `AurOS-Installer-test.exe` from branch
`image-desktop` (SHA-256 `3e32e929…3e875`, built from
`claude/laughing-cray-ayao6i` at `cd437e1`) was downloaded onto a real
Windows PC for the first time. **It never started.** Nothing on the PC
was changed: Windows refused to load the program before any of its code ran.

Before that, the download had been checked from the Linux side: its
SHA-256, that it is a 64-bit Windows program, and every image piece it
downloads. All of those passed. None of them could show whether Windows
would start the program. That gap is issue 3.

## What the tester saw, in order

1. **Avast: "Suspicious file detected".** It scanned the file and
   uploaded it to Avast Threat Labs.
2. **Avast: "Hmm… This needs a closer look".** Avast said it would keep
   blocking the file for "a few hours" while it analysed it.
3. **Windows: "The application has failed to start because its
   side-by-side configuration is incorrect."** This happens on every
   run, whatever Avast decides.

## The issues, in the order to fix them

| # | File | What | Severity |
|---|------|------|----------|
| 1 | [01-sxs-manifest.md](01-sxs-manifest.md) | The embedded manifest isn't valid XML, so Windows won't load the program. **Root cause confirmed.** | **Blocker.** Nobody can run it. |
| 2 | [02-antivirus-block.md](02-antivirus-block.md) | Avast flags and blocks the installer. Other antivirus products probably will too. | **Blocker** for the audience. It needs a workaround for testers now and a real fix before release. |
| 3 | [03-testing-gap.md](03-testing-gap.md) | No test ever started the program on real Windows. Wine accepted the broken manifest. | **Process.** This is why 1 got out. |
| 4 | [04-republish.md](04-republish.md) | What has to be rebuilt, re-hashed and re-documented once 1 is fixed. | Follow-up |

## One-line summary for whoever fixes this

`src/aurbridge/aurbridge.manifest` line 7 (on `claude/laughing-cray-ayao6i`)
has `--` inside an XML comment. XML forbids that, and Windows' manifest
parser is strict about it. Remove it, add a check to the build that parses
the manifest strictly, and add a test that starts the program on real
Windows before anything is published again.
