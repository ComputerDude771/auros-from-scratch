# 1. "Side-by-side configuration is incorrect": the manifest isn't valid XML

**Severity:** blocker. The installer can't start on any Windows PC.
**Status:** root cause confirmed. Not fixed.

## What happens

Double-clicking `AurOS-Installer-test.exe` shows:

> The application has failed to start because its side-by-side
> configuration is incorrect. Please see the application event log or use
> the command-line sxstrace.exe tool for more detail.

Windows shows this while it is still creating the process, before any
AurBridge code runs. The wizard never opens, the "allow this app to make
changes" prompt never appears, and **nothing on the PC is touched**.

## Root cause

The wizard is linked with an application manifest (resource type 24,
`RT_MANIFEST`, id 1). It is built from `src/aurbridge/aurbridge.manifest`
by `build/aurbridge` (lines ~207–218: a `.rc` file, then `windres`). The
manifest starts with a long explanatory XML comment, and line 7 of that
comment ends with a double hyphen:

```xml
<!--
  ...
  page whose remedy was "right-click and choose Run as administrator" --
  a step the people this is for do not know exists. ...
-->
```

**XML doesn't allow `--` anywhere inside a comment** (XML 1.0 §2.5).
Windows' side-by-side manifest parser is a strict XML parser, so it
rejects the whole manifest. Windows can't build an activation context
for the program, so it refuses to start it. If the program is launched
from code instead of a double-click, `CreateProcess` fails with error
14001 (`ERROR_SXS_CANT_GEN_ACTCTX`).

### Proof

The manifest was extracted from the published `.exe` itself, not taken
from the source tree:

```
$ python3 -c "import pefile; ..."   # RT_MANIFEST id 1, lang 1033, 1699 bytes
$ xmllint --noout manifest.xml
manifest.xml:7: parser error : Double hyphen within comment
$ python3 -c "import xml.etree.ElementTree as E; E.parse('manifest.xml')"
xml.etree.ElementTree.ParseError: not well-formed (invalid token): line 7, column 72
```

The rest of the manifest is fine: `requireAdministrator`, the four
`supportedOS` GUIDs (Windows 7 to 11) and `processorArchitecture="amd64"`
are all correct. The only problem is the `--`.

### Why no test caught it

Every test of the Windows half runs under Wine (`docs/RELEASE.md` §5:
"has only run under Wine and against a simulated machine"). Wine's
manifest handling is lenient, so the file loaded there. See
[03-testing-gap.md](03-testing-gap.md).

### Which builds are affected

- The manifest came in with commit `e887aec` ("aurbridge: the no-stick
  install…") and hasn't changed since. So the published test installer
  (`3e32e929…`) is broken. The earlier no-stick installers listed in the
  `image-desktop` README (`540a0bf2…`, `005fe4d8…`) almost certainly are
  too. That wasn't checked, because those files are no longer on the branch.
- `aurbridge.exe`, the console tool, is built **without** the manifest
  (`build/aurbridge` says so on purpose), so this bug doesn't affect it.
  It isn't the file that was published, though.
- The original line, `claude/linux-distro-from-scratch-tt5dx1`, has no
  `aurbridge.manifest` at all.

## What needs to be fixed

1. **Remove the `--` from the comment** in `src/aurbridge/aurbridge.manifest`.
   A single hyphen, a colon or a full stop will do. It's safer still to move
   the explanation out of the manifest altogether (into `build/aurbridge` or
   `docs/AURBRIDGE.md`). Everything in the manifest ships byte for byte
   inside the `.exe`, and any comment there can carry the same trap again.
2. **Make the build refuse a malformed manifest.** In `build/aurbridge`,
   before `windres`, parse it strictly and `die` if it fails, for example
   `xmllint --noout "$SRC/aurbridge.manifest"` or a `python3 -c` using
   `xml.etree`. Prove the check works by putting `--` back and watching the
   build stop (the project's rule: a check isn't trusted until it has been
   seen to fail).
3. **Check what actually shipped, not just the source.** After linking,
   extract `RT_MANIFEST` from `out/aurbridge-wizard.exe` and parse that too,
   so a quoting or encoding problem introduced by `windres` or the `.rc`
   file is also caught.
4. **Start the program on real Windows before publishing.** See
   [03-testing-gap.md](03-testing-gap.md). Neither the hash nor Wine can
   show that Windows will load the file.

## If a tester wants the detail from Windows

In an administrator Command Prompt:

```
sxstrace trace -logfile:sxs.etl
  (double-click the installer, dismiss the error, press Enter in the prompt)
sxstrace parse -logfile:sxs.etl -outfile:sxs.txt
```

`sxs.txt` should report a manifest parse error at line 7. The same
failure is also written to Event Viewer → Windows Logs → Application,
source **SideBySide**, event ID 72 or 59.
