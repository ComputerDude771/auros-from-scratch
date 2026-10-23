# Reporting a security problem in AurOS

AurOS is used by people who trust it with the computer that holds their
photos, their email and their bank's website. If you have found a way
that AurOS, its installer or its website could be used to harm them,
please tell us privately first, so it can be fixed before it is
public.

## How to report

**Use GitHub's private reporting:** open
<https://github.com/ComputerDude771/auros-from-scratch/security/advisories/new>
(or the repository's **Security** tab, then **Report a vulnerability**).
Only the maintainers can see what you write there.

Please **do not** open a public issue, pull request or discussion for
a security problem.

> Maintainer note: GitHub's private vulnerability reporting must be
> switched on once, in **Settings → Code security → Private
> vulnerability reporting → Enable**. Until it is, the link above says
> the feature is off; in that case write to the security contact listed
> here: **[OWNER: add a security contact address before launch]**.

Helpful things to include, as far as you can:

- what part is affected (the Windows installer `AurOS-Setup.exe`, the
  restart and set-up stage, the AurOS desktop, the update channel, the
  website, the build or release workflows);
- the version or release tag, and the computer it happened on;
- the steps to make it happen, and what an attacker gains;
- whether it is already public or being used against people.

Never send us anyone's real passwords, recovery keys or personal files.
A description, a test machine or a made-up example is enough.

## What happens next

These are targets, not promises, for a small volunteer project:

| When | What |
|---|---|
| within 3 working days | we say we have your report |
| within 10 working days | we say whether we can reproduce it, and how serious we think it is |
| as soon as a fix is ready | a fixed release; for the installed system, through AurOS's own update channel |
| after the fix is out (normally within 90 days of your report) | a public advisory, crediting you if you want to be named |

If we disagree with you about how serious it is, we will say why. If a
problem is being used against people right now, we will move faster
and may publish advice before the fix.

## What counts

In scope, for example:

- anything that lets the installer damage, expose or lose data on the
  Windows side beyond what the person agreed to (it must never touch
  anything outside the space it was given, and never decrypt BitLocker);
- anything that lets another program, a website, a network neighbour or
  a USB device gain control of an AurOS computer, or of the person's
  files, without them knowing;
- tampering with downloads, release files or updates that the installer
  or AurOS would accept;
- a way past the disk encryption and PIN, once that ships;
- the website serving or linking to something harmful.

Out of scope (please report these to the project that owns them):
problems in Ubuntu packages, Firefox, Flathub programs, Windows or the
computer's firmware that AurOS ships unchanged. If you are not sure who
owns a problem, report it here and we will pass it on.

## Safe harbour

If you act in good faith, stay within your own devices and accounts,
avoid harming anyone's data or privacy, and give us a reasonable chance
to fix the problem before you publish, we will not pursue or support
legal action against you for your research.

> **[OWNER: this safe-harbour paragraph is a promise with legal weight.
> Have it reviewed together with the drafts in `docs/legal/` before
> launch.]**

## Supported versions

Only the newest release (marked "Latest" on the
[releases page](https://github.com/ComputerDude771/auros-from-scratch/releases))
and the AurOS systems installed from it, kept up to date through
AurOS's update channel, receive security fixes. Pre-releases are for
testing and are not supported.
