# AurOS privacy policy

> **DRAFT: must be reviewed by a lawyer before use.**
> Not legal advice. Not in force. Placeholders marked `[OWNER: ...]`;
> questions for the lawyer marked `[LAWYER: ...]`; facts an engineer must
> re-check before publishing marked `[CHECK: ...]`. See `README.md`.

Last updated: [OWNER: date of approval]

Who is responsible: [OWNER: legal name, address, contact for privacy
questions] ("we", "us").

## 1. In short

- **We don't collect anything about you.** AurOS has no account, no
  sign-in, no adverts, no analytics and no tracking, on the website, in
  the installer, or on your computer.
- **Nothing on your computer is sent to us.** Your files, your
  passwords and what you do stay on your computer.
- To download AurOS and keep it safe, your computer does talk to some
  **other organisations' servers** (GitHub, Ubuntu, Mozilla, Flathub,
  Google's Safe Browsing). They see what any website you visit sees,
  such as your internet address. Section 4 lists them.

## 2. The website

The website (computerdude771.github.io/auros-from-scratch) uses no
cookies, no analytics and no tracking, and loads nothing from any other
server: its fonts and pictures come from the same place as its pages.
If you press the light/dark switch on some pages, your browser remembers
that choice in its own storage on your device; it is not sent anywhere.

The website is hosted by **GitHub Pages** (GitHub, Inc., owned by
Microsoft). Like any web host, GitHub receives your internet (IP)
address and browser details when you visit, and may keep them in its
logs, under GitHub's own privacy statement. We do not get these logs.
[LAWYER: does using GitHub Pages make us a controller or joint
controller for these logs, and what must be said? CHECK: GitHub's
current Pages logging statement.]

## 3. The installer (AurOS-Setup.exe)

The installer runs on your Windows computer. To decide whether AurOS can
go on it safely, it **looks at your computer**: the Windows version, the
kind of firmware, how the drive is divided and how much space is free,
whether the drive is encrypted, the drive's health, and similar facts.
**It looks at these on your computer and does not send them anywhere.**

It downloads AurOS from **GitHub Releases** (see section 4), which sees
your internet address like any download.

If you agree on screen ("Bring my Wi-Fi across"), it reads the name and
password of the Wi-Fi network your computer is using, so AurOS can get
online on its first start. The password is only ever **copied from your
Windows to your new AurOS on the same computer**: it is never sent over
the internet, and the temporary copy used during the restart is deleted
once AurOS has stored it. [CHECK: once D10 ships, confirm the copy is
deleted in every path, including a failed install.]

The installer keeps a log of what it did on your computer, so that a
problem can be understood. [CHECK: the log's location and whether it
contains any personal data such as user or network names.] We only see
it if you choose to send it to us.

## 4. AurOS on your computer

AurOS doesn't send us anything: no usage statistics, no crash reports,
no list of your programs. Ubuntu's own "popularity contest" and its
"Ubuntu Pro" software are left out of AurOS on purpose.

To keep your computer safe and working, AurOS and the programs it
comes with connect to these services while you are online. Each sees
your internet address and what is being downloaded, under its own
privacy policy:

| Service | Run by | Why AurOS uses it |
|---|---|---|
| Ubuntu's software archive | Canonical Ltd | security updates for the system |
| Mozilla's software archive | Mozilla | Firefox and its updates |
| Firefox's own services | Mozilla | Firefox add-on updates; uBlock Origin from addons.mozilla.org. Firefox's usage statistics (telemetry) are switched off. |
| Safe Browsing (inside Firefox) | Google | warns before a dangerous website or download. Firefox checks addresses against a list it downloads; for some downloads it sends details of the file (not the file itself) to Google to check it. |
| Flathub | Flathub (GNOME Foundation and partners) | the catalogue and downloads in **Get more programs** |
| GitHub Releases | GitHub, Inc. | AurOS's own updates [CHECK: once the update channel in docs/security/UPDATES.md ships] |
| [CHECK: time server, e.g. ntp.ubuntu.com] | [CHECK] | keeping the clock right |
| [CHECK: NetworkManager connectivity check, if enabled] | [CHECK] | telling whether the internet works |
| [CHECK: app reviews in Get more programs (ODRS), if enabled] | [CHECK] | showing other people's reviews of programs |

[CHECK before publishing: list every host an idle AurOS computer and a
first start contact, by watching its connections in a test machine
(for example, a packet capture on the CI virtual machine), and make this
table match. Re-check whenever a release adds a service.]

**Programs you add** through Get more programs, and **websites you
visit**, have their own privacy policies; we don't control what they
collect.

### Your files from Windows

On its first start, AurOS offers to **bring copies of your documents,
pictures and browser bookmarks across from Windows**. This copying
happens entirely on your computer, from one part of the drive to
another. Nothing is uploaded. Passwords saved in your Windows browser
are never copied.

### Disk encryption and PIN

[CHECK: once the encrypted disk and PIN (owner decision D9) ship,
describe here: the PIN never leaves the computer; the recovery key is
shown once for you to keep and is not sent to us.]

## 5. If you contact us

If you write to us, or open an issue on our project page on GitHub,
we see what you write and the name you use there. Issues on GitHub are
**public**. We use what you send only to answer you and to fix AurOS.
[OWNER: how long you keep emails or private reports.] Please never send
passwords, recovery keys or private files.

## 6. Children

AurOS is not aimed at children and collects nothing from anyone.
[LAWYER: is any statement needed?]

## 7. Your rights

Because we don't collect personal data about people who use AurOS, there
is usually nothing for us to show, correct or delete. If you think we
hold something about you (for example an email you sent), ask us at
[OWNER: contact] and we will tell you and, if you wish, delete it.
[LAWYER: rights wording for GDPR/UK GDPR, California and other places
where users live; the supervisory authority to name.]

## 8. Changes

If this policy changes, the new version will be on the website with a
new date, before the change takes effect. If AurOS ever starts to send
anything new about your computer to anyone, we will say so **before**
the update that does it.

> **DRAFT: must be reviewed by a lawyer before use.**
