# Owner setup: every click and secret only the repo owner can do

The agents can write code, push, run CI and publish releases. They cannot
open the repository's Settings pages, so everything below needs the
owner, once per repository. **No secret value is ever stored in this
repository** — this file only says what each secret is, where it goes,
and how to make it.

Replace `OWNER/REPO` with the repository (today:
`ComputerDude771/auros-from-scratch`).

## 1. Turn on the website (2 minutes, needed now)

1. `https://github.com/OWNER/REPO/settings/pages` → **Build and deployment** →
   **Source: GitHub Actions**.
2. `https://github.com/OWNER/REPO/settings/environments` → **github-pages** →
   **Deployment branches and tags**: choose **No restriction**, or add a rule
   for the working branch (`claude/brave-hypatia-x72g1m` today).
3. The site goes live on the next release (the release workflow deploys it),
   at `https://OWNER.github.io/REPO/`.

## 2. Make the working branch the default (1 minute, recommended)

`https://github.com/OWNER/REPO/settings` → **Default branch** → switch to the
working branch. Why: GitHub only shows a "Run workflow" button for workflows
on the default branch, and the Pages environment trusts the default branch
by default. Today the default is `claude/linux-distro-from-scratch-tt5dx1`,
an old branch.

## 3. Workflow permissions (check once)

`https://github.com/OWNER/REPO/settings/actions` → **Workflow permissions**:
"Read and write permissions" is not required (each workflow asks for exactly
what it needs), but **"Allow GitHub Actions to create and approve pull
requests" can stay off**. If a release job fails with "Resource not
accessible by integration", this page is where to look.

## 4. Secrets (Settings → Secrets and variables → Actions → New repository secret)

`https://github.com/OWNER/REPO/settings/secrets/actions`

| Secret name | What it is | When it's needed | How to make it |
|---|---|---|---|
| `AUROS_APT_SIGNING_KEY` | the private half of AurOS's own update-signing key (OpenPGP). Computers running AurOS trust only updates sealed with it. **Not Microsoft, not a certificate, free.** | when the update channel (stream S5) goes live | the exact commands are added here by stream S5; in short: make a key pair on your Mac, paste the private key here, the public half goes into the repo |

### 4a. `AUROS_APT_SIGNING_KEY`, step by step (once, about 5 minutes)

This is the key that seals AurOS's own updates (stream S5,
`docs/security/UPDATES.md`). Make it on **your own computer**, never on a
shared or cloud machine. You need GnuPG: on Linux it is already there
(`gpg --version`); on a Mac, `brew install gnupg` (or GPG Suite).

1. Open a terminal and paste these lines (they make the key in a fresh,
   temporary folder, so nothing mixes with any key you already have):

   ```sh
   export GNUPGHOME="$(mktemp -d)"
   gpg --batch --passphrase '' --quick-gen-key 'AurOS updates' ed25519 sign 3y
   gpg --list-keys --with-colons | awk -F: '/^fpr:/{print $10; exit}'
   gpg --armor --export-secret-keys > auros-apt-signing-key.PRIVATE.asc
   gpg --armor --export             > auros-archive-keyring.asc
   ```

   The third line prints the key's **fingerprint** (40 letters and digits).
   Write it down.
2. Open `https://github.com/OWNER/REPO/settings/secrets/actions` →
   **New repository secret**. Name: `AUROS_APT_SIGNING_KEY`. Value: open
   `auros-apt-signing-key.PRIVATE.asc` in a text editor and paste **all**
   of it, from `-----BEGIN PGP PRIVATE KEY BLOCK-----` to the matching
   `END` line. **Add secret.**
3. Copy both `.asc` files to a USB stick and put it somewhere safe. Anyone
   holding the PRIVATE file can send updates to every AurOS computer: treat
   it like the master key of a house. You need it again only to move to a
   new repository (paste the same file into the new repository's secret)
   or to renew the key before it expires in 3 years.
4. Delete the copies from the computer: `rm -rf "$GNUPGHOME"
   auros-apt-signing-key.PRIVATE.asc` (keep `auros-archive-keyring.asc` if
   you like; it is the public half and is safe to share).
5. Tell an agent: "the update key is in, fingerprint XXXX". The next
   `[apt]` run publishes the public half; the agent commits it as
   `rootfs/usr/share/keyrings/auros-archive-keyring.asc` after checking it
   has your fingerprint. From the next image on, AurOS computers trust
   updates sealed with it, and only those.

Nothing else is needed: no certificate, no company, no Microsoft account,
no payment.

Rules for every secret:
- Paste it only into that Settings page. Never into a file, an issue, a chat, or a commit.
- Keep an offline copy (a USB stick in a drawer) of the update-signing key: if it is lost,
  existing AurOS computers can no longer be updated without a new install.
- If it may have leaked: make a new key, replace the secret, and publish an update signed
  with the old key that installs the new public key (stream S5 documents this rotation).

## 5. Not required, by the owner's decision

- **No code-signing certificate, no Azure, no Microsoft account.** The installer
  is unsigned by design; the website shows people the two extra clicks.

## 6. When you move to a new repository

See `docs/MOVING.md`: one `git clone` brings every file; then redo sections
1, 2 and 4 on the new repository, and push once to rebuild the release.
