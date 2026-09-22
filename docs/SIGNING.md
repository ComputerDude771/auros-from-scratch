# Signing — the one thing in this product that has to be bought

> Everything else in this repository can be built, tested and fixed by
> reading code. This cannot. A code-signing certificate is a legal
> identity check performed by somebody else, and the price of not
> having one is that the people this product was built for cannot start
> it.

---

## What an unsigned installer looks like to the person it is for

She downloads one file and double-clicks it. Windows SmartScreen puts a
**full-screen blue panel** in front of it:

> **Windows protected your PC**
> Microsoft Defender SmartScreen prevented an unrecognised app from
> starting. Running this app might put your PC at risk.
> `[ Don't run ]`

The only button she can see says *Don't run*. There is a **More info**
link above it, in the same colour as the text around it, which reveals
a *Run anyway* button. Most people never press it — and the ones who do
have just been taught to click past a security warning immediately
before handing a program their entire disk. That is a worse outcome
than the first one.

So an unsigned AurBridge is not a product with a rough edge. It is two
different failures, and neither of them is fixable in code.

---

## What has to be bought

**An OV or EV code-signing certificate, on hardware.**

Since 1 June 2023 the CA/Browser Forum's baseline requirements have
required code-signing private keys to be generated and held on
hardware meeting **FIPS 140-2 Level 2** (or Common Criteria EAL4+).
The practical consequence is that **no public CA will sell you a `.pfx`
file any more.** Every issuance now arrives as one of:

| | What it is | What signing looks like |
|---|---|---|
| **USB token** | a SafeNet eToken 5110 posted to you | plug it in; sign locally; the PIN never leaves it |
| **Cloud HSM** | a signing service holding the key for you | authenticate over the network; the file is hashed locally and the *hash* is signed remotely |

Either works with this build. The cloud route is easier to automate and
does not put a physical object on the critical path of a release; the
token is cheaper and does not depend on somebody else's uptime.

### OV or EV

|  | OV | EV |
|---|---|---|
| Identity check | the organisation is verified | the organisation is verified more strictly |
| SmartScreen | reputation is **earned** — the warning keeps appearing until enough people have downloaded and run it | **immediate** — no warning from the first download |
| Typical price | roughly USD 200–400 a year | roughly USD 300–600 a year |

Check current prices; they move, and resellers are usually well under a
CA's list price.

**For this product, EV is the one to buy, and the reason is specific.**
OV reputation is accrued per-publisher across downloads over time. A
new publisher signing a new installer still gets the warning — for
weeks, or until a few thousand people have run it. Those weeks are
exactly the launch, and the population is exactly the one that cannot
get past a blue screen. Paying the difference buys the absence of the
warning on the very first download, which is the only download that
matters.

### Who can buy one

OV and EV are issued to a **verified legal entity**: a registered
company, a charity, a partnership, or in some jurisdictions a sole
trader with the right documents. Several CAs will issue to an
individual, with a heavier identity check (government ID plus a
verifiable public record of address). If AurOS is going to be
downloaded by strangers, registering something is worth doing anyway —
the certificate's subject name is what they will see in the UAC prompt,
and "AurOS Ltd" reads very differently from a personal name.

### Where to buy

Any public CA that issues code-signing certificates: DigiCert, Sectigo,
GlobalSign, SSL.com, Certum. Certum's *Open Source Code Signing* is
substantially cheaper than the others and is issued to individuals
working on open-source projects; it is OV, so the SmartScreen paragraph
above applies. Resellers (SSLs.com, Cheapsslsecurity, and others) sell
the same CAs' certificates for much less than list.

---

## What the build already does

`build/sign` is written, tested, and waiting for a key.

```sh
# a hardware token
export AUROS_SIGN_PKCS11_MODULE=/usr/lib/pkcs11/libeTPkcs11.so
export AUROS_SIGN_PKCS11_CERT='pkcs11:object=my-cert;type=cert'
export AUROS_SIGN_PKCS11_KEY='pkcs11:object=my-key;type=private'
export AUROS_SIGN_PASS_FILE=~/.auros-token-pin      # mode 0600

# or a cloud HSM that presents a PKCS#12 bridge
export AUROS_SIGN_PKCS12=/path/to/signing.p12
export AUROS_SIGN_PASS_FILE=~/.auros-p12-pass

AUROS_RELEASE=1 ./build/aurbridge
```

It signs with SHA-256, timestamps against an RFC 3161 server, and then
**verifies what it just wrote**. That last step is the one usually left
out, and the first person to discover a signature did not take should
not be a stranger looking at a blue screen.

`AUROS_RELEASE=1` makes an unsigned build a **refusal** rather than a
warning. A developer build without a token still works and says what is
missing, because requiring a hardware token to compile is how nobody
compiles.

### Timestamping is not optional

A signature without a timestamp stops validating the day the
certificate expires — every copy ever downloaded, all at once. A
timestamped one keeps validating for ever, because the timestamp proves
the signing happened while the certificate was valid. `build/sign`
timestamps by default and shouts on every run if told not to.

### Nothing in this repository holds a key

Configuration is environment variables only. A signing key committed to
a repository is a signing key on every laptop that ever cloned it —
and with the hardware requirement above there is usually no key file to
commit, which is the point of the requirement.

---

## The other signature: Secure Boot

This is a separate question with a much happier answer, and it is worth
saying why it is already solved.

AurOS boots through **Canonical's `shimx64.efi.dualsigned`** and their
signed `grubx64.efi`, copied out of the image by `build/mkimage` — not
rebuilt. Those binaries carry Microsoft's signature already, so AurOS
boots on a machine with Secure Boot enabled without anybody submitting
anything to anybody. `tools/loadertest.sh` proves it against
`OVMF_CODE_4M.ms.fd` with Microsoft's own keys enrolled and Secure Boot
enforcing.

The shim is **dual-signed**: by the Microsoft UEFI CA 2011 *and* the
2023 CA. The 2011 CA is the one old firmware trusts, and it is the
whole reason the chain is borrowed rather than built — the dual-signing
window closed in June 2026, so a distribution applying to Microsoft on
its own today can only obtain a 2023-only shim, which would not boot on
exactly the old PCs this product exists for.

If AurOS ever needs its own shim — to carry its own vendor certificate,
so that kernel modules could be signed by us — that is a submission to
the Microsoft UEFI signing service through a shim review, it takes
months, and it does not remove the 2011 problem. There is no reason to
start it, and this paragraph exists so that nobody starts it by
accident.

---

## What is still missing after the certificate arrives

1. **The certificate's subject name is what the UAC prompt says.** Look
   at it once in a VM before publishing. "Unknown publisher" and a
   company name are different products.
2. **Renewal.** A year is short. The timestamp means old downloads keep
   working; new builds stop being signable the day it lapses.
3. **Revocation.** If the token is lost, the certificate is revoked and
   every timestamped binary signed before the revocation date keeps
   working. That is the other half of why timestamping matters.
