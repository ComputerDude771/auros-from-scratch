# 11. Non-code blockers to any public download

**Source:** `docs/RELEASE.md` §1–6, `docs/SIGNING.md`. None of these can
be fixed by writing code. They're listed so the v3 list is complete.

| # | Blocker | Why it matters | What unblocks it |
|---|---------|----------------|------------------|
| 11.1 | **A legal entity** | The code-signing certificate is issued to it, and the licence and insurance are with it. "This product repartitions consumer disks; somebody will lose data." | Register a company (US LLC, UK Ltd, or the local equivalent), with its name and address the same everywhere. |
| 11.2 | **Code signing** | An unsigned `.exe` gets SmartScreen's "Windows protected your PC", is blocked outright by Smart App Control, and looks suspicious to antivirus ([02](02-antivirus-block.md)). | Azure Artifact Signing (~USD 9.99/month, if eligible) or an OV certificate (~USD 200–400/year). **Not EV**: since 2024 it no longer skips SmartScreen. Even signed, SmartScreen warns until the file earns reputation. |
| 11.3 | **An image host** | See [07](07-images-and-build.md) §7.3. | GitHub Releases or R2/B2 with a CDN. |
| 11.4 | **A licence, disclaimer and support path** | Somebody has to answer when a stranger's disk goes wrong. | A lawyer-drafted EULA where the entity is registered, and a support address somebody reads. The wizard's consent page and `website/safety.html` should say the same things. |
| 11.5 | **Insurance** | Professional liability (technology errors and omissions). | Through a broker, once 11.1 exists. |
| 11.6 | **A tested hardware list** | See [09](09-real-hardware.md). | The dry-run fleet. |

**Order:** 11.1 comes before 11.2 and 11.4. 11.3 and the hardware work
in 11.6 can start today.
