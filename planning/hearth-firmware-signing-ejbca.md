# Issuing the firmware signing certificates in EJBCA

Written 2026-10-25 for the owner, for [the signed update index of O10](esp32-ota.md#trust-and-signing). It is a
step-by-step for EJBCA's web pages, with the exact fields. It says what to set and why, in the order to do it.
Nothing in it needs a key to be shown to anyone: **never paste, send or screenshot a private key, a PFX, a
password, an enrollment code or a crypto token's activation code**, in a chat or anywhere. Public certificates
are fine to share.

It was corrected the same day against what was really issued (EJBCA 7.11.0 Community), and
[As built](#as-built-2026-10-25) at the end lists the real names, dates and fingerprints. The profile names below are
working names. On the owner's EJBCA the Windows pair is the certificate profile `CODESIGNER` and the end entity
profile `local code signing`, and the copies were named `FIRMWARE-SUBCA`, `FIRMWARE-CODESIGNER` and
`Firmware CodeSigning`; use the names you choose.

What this makes: **one firmware sub-CA** and **two leaf certificates** under it, `Hearth Firmware Stable` and
`Hearth Firmware Beta`. CI signs with the two leaves (one in each GitHub environment). The board holds the
sub-CA's public certificate, so a Windows signing certificate, or any other certificate your CA ever issued,
cannot sign firmware.

## What this is based on

- The Windows test-signing certificate (the shared actions repository `icl-common-infra`: its
  test-signing page and its `issue-leaf.sh` script) is: `CA:FALSE`, key usage `digitalSignature`,
  extended key usage `codeSigning` (1.3.6.1.5.5.7.3.3), RSA 3072, SHA-256, at most 365 days, its common name
  "Iain Chesworth Labs Test Signing", kept as a PFX in organisation secrets and used by CI with `signtool`. The
  firmware leaves are the same kind of certificate, so the existing code signing profile pair already describes
  them. That document describes an openssl script as the issuer; on the owner's EJBCA the pair is what issues such a
  certificate (RSA, one year, extended key usage not critical, available to the Intermediate CA only).
- The board checks: the chain ends at the sub-CA it holds; the leaf is for code signing (the extension is present;
  its criticality is not looked at); the leaf's common name is the pinned one for the channel; the signature. It
  checks no CRL, no OCSP and no timestamp, and it never looks above the sub-CA.

## 0. Decisions in this guide

| Question | Answer | Why |
|---|---|---|
| Sub-CA, or the root with the common name pinned? | **A sub-CA** | Whatever else your CA ever issued (the Windows certificate, one made by mistake) then cannot sign firmware, and the pin is a second lock instead of the only one. EJBCA, Community and Enterprise, can make sub-CAs. The signer can be the root or an Intermediate CA, since the board never looks above the sub-CA. If the signer's key is not held by EJBCA (an imported or offline CA), use step 2's external route; if even that is more than you want today, the root with a pin works and the board needs no change |
| Key type | **RSA 3072, SHA-256** (the same as the Windows leaf) | The profile pair you have already issues exactly this, so nothing in EJBCA needs a new algorithm enabled. A board already carries RSA for TLS (GitHub's CDN uses an RSA certificate). ECDSA P-256 would make the chain about 2 KB smaller; choose it only if your profile already lists it, since a board handles either |
| Signature scheme | RSA PKCS#1 v1.5 with SHA-256 | What every tool produces without options. The tool also supports PSS if you ever prefer it |
| Validity | **Two years** for the leaves; for the sub-CA ten years or what its signer has left, whichever is less | EJBCA will not make a CA that outlives its signer (the owner's chain ends 2035-07-25, so the sub-CA got 8 years). A board judges a leaf's dates only when its clock is set. The CI job warns 60 days before a leaf ends. Renewing is a new leaf with the same name; no board changes |
| Extensions | Key usage `digitalSignature` only and basic constraints `CA:false`, both critical; extended key usage `codeSigning` only, as the Windows profile has it (**not critical**) | Nothing else can use the key. No tool checks the criticality of the extended key usage, only that code signing is in it |
| CRL, OCSP, AIA | **Leave out** of the firmware profiles | The board checks none of them, and each one adds bytes and a URL to every signature file |
| Who makes the key | **You, on your machine, with a certificate request (CSR)** | The private key never reaches EJBCA. The only place it travels to is the GitHub environment secret, which is where CI has to read it |
| Separate profiles | **Copies** of the Windows pair, restricted to the one sub-CA | Your Windows profiles stay as they are |

## 1. Look at what you have (screenshots, before changing anything)

Send these to the session, so that it can check them against this guide before anything is issued. Each is a
page of settings, not a secret. Crop out anything that shows a key, a password or a code.

1. **The version.** The footer or the About page of the admin web, showing the EJBCA name and version.
2. **CA Functions, Certification Authorities.** The list: names and status.
3. **The CA that will sign the sub-CA, and the CAs above it, viewed** (select each, **Edit CA**, then leave with
   Back, not Save): subject DN, signing algorithm, validity, "signed by", and its crypto token. Or send each one's
   public certificate and the session reads it with `openssl`. Also check on **Crypto Tokens** whether the signer's
   token is active (step 2 needs it for a minute).
4. **CA Functions, Certificate Profiles:** the list, then the code signing profile opened with **View** (a profile
   that cannot be edited, like `SUBCA`, has no Edit): every field from the top to the end, in as many screenshots as
   it takes. The ones that matter: Type; Available Key Algorithms; Available Bit Lengths; Signature Algorithm;
   Validity; Basic Constraints; Key Usage; Extended Key Usage; CRL Distribution Points; Authority Information
   Access; Available CAs; Publishers.
5. **RA Functions, End Entity Profiles:** the code signing end entity profile, from the top (or simply clone it
   first and send the copy's Edit page, before you change anything): Subject DN Attributes; Subject Alternative Name;
   Main Certificate Data (Default Certificate Profile, Available Certificate Profiles, Default CA, Available CAs,
   Default Token, Available Tokens); the Other Data section. Hide a username or enrollment code if one is shown.
6. **The Windows certificate, as issued** (optional once the profile in 4 is seen): the output of
   `openssl x509 -in <that certificate> -noout -text`.

## 2. Make the firmware sub-CA

*CA Functions, Certification Authorities.* These steps were done on EJBCA 7.11.0 Community; menu names differ a
little in 8 and 9, so if a field below is not where it says, send a screenshot of the page.

1. **A certificate profile for the sub-CA.** *CA Functions, Certificate Profiles*: **Clone** `SUBCA` (it cannot be
   edited, but it can be cloned) and name the copy (`FIRMWARE-SUBCA`). **Edit** the copy:
   - Type: **Sub CA**.
   - Available key algorithms: **RSA** only; bit lengths **3072** only. Signature algorithm: leave **Inherit from
     issuing CA** (the signer's algorithm signs this one certificate, and the board never checks it).
   - Validity: ten years, or what the signer has left if that is less (**`8y`** on the owner's EJBCA). Leave **Allow
     Validity Override** ticked, as `SUBCA` has it, so that the CA page's own validity applies.
   - Basic Constraints: **Use, Critical**, and the **Path Length Constraint** ticked with the value **0**. That
     stops the sub-CA from making another CA.
   - Authority Key ID and Subject Key ID: **Use**.
   - Key Usage: **Use, Critical, `Key certificate sign` and `CRL sign` only** (untick Digital Signature if it is on).
   - Subject Alternative Name and Issuer Alternative Name: **untick Use** (`SUBCA` has both on).
   - Extended Key Usage, CRL Distribution Points, Authority Information Access, OCSP, Certificate Policies, Name
     Constraints: not used.
   - Available CAs: **only the CA that will sign** (not "Any CA"). **Save**.
2. **A crypto token for the sub-CA's key.** *CA Functions, Crypto Tokens, Create new*: type **Soft**, a name for the
   CA, an authentication code of your own (never shown to anyone), **auto-activation off** (the key then stays
   locked, and you activate the token only to issue or renew a leaf; tick it instead if you want the CA always
   available), export of private keys not allowed. **Save**, then on the token's page **generate two key pairs**:
   `signKey` and `encryptKey`, both **RSA 3072** (the second is only EJBCA's own bookkeeping, and 3072 keeps the
   profile's one-size rule simple).
3. **Add the CA.** *Certification Authorities*, name `Iain Chesworth Labs Firmware CA`, **Create...**. Choose the
   crypto token first, which fills the key lists, then fill in:
   - CA type X.509. Key aliases: certSignKey `signKey` (crlSignKey follows it); defaultKey, keyEncryptKey and
     testKey `encryptKey`.
   - Signing algorithm: **SHA256WithRSA** (this is what signs the leaves).
   - Description: "Signs Hearth firmware update leaves only".
   - Directives: leave **Enforce unique public keys** on, and **untick Enforce unique DN** (a renewal reuses the
     name).
   - Subject DN: `CN=Iain Chesworth Labs Firmware CA,O=Iain Chesworth Labs`. The box is narrow: check that it shows
     the whole text.
   - **Signed by**: the root CA or an Intermediate CA, as you prefer. The CA that signs needs its **crypto token
     active** for this step: if it is not, EJBCA says "The keys in the crypto token with id ... could not be
     accessed". Activate it on *Crypto Tokens*, create the CA, then **deactivate it again**. If the signer's key is
     not held by this EJBCA, choose **External CA** instead: EJBCA gives you a request (CSR) for the sub-CA, you sign
     it with your CA wherever its key is, with the same profile settings (CA, path length 0, key usage keyCertSign
     and cRLSign), and import the signed certificate with the chain.
   - Certificate profile: the sub-CA profile above. Validity: the same as the profile (`8y`).
   - **Untick "Monitor if CA active"** while the token is not auto-activated, so that the health check does not
     call a locked CA a fault. Leave Microsoft CA Compatibility Mode off (it cannot be undone), and the rest at its
     defaults. **Create**. The CA is listed as `Active`.
4. **Download the sub-CA certificate** (*Certification Authorities*, the CA, **Edit CA**, **View Certificate**, then
   its PEM download link; or the admin home page's *CA certificates and CRLs*). This file is public; it is the
   anchor in the board's firmware. Leave Edit CA with Back, not Save. Keep the file as `firmware-ca.pem`.
5. **Check it** (a public certificate, safe to run and to paste): `openssl x509 -in firmware-ca.pem -noout -subject
   -issuer -dates -ext basicConstraints,keyUsage`. You want `CA:TRUE, pathlen:0` (critical), `Certificate Sign, CRL
   Sign` (critical), no other extension of interest, and the dates the profile allows. Then
   `openssl verify -CAfile <root certificate> -untrusted <intermediate certificate> firmware-ca.pem` prints `OK`
   (leave out `-untrusted` if the root signed it).

## 3. Make the two profiles for the leaves

1. **Certificate profile.** **Clone** your code signing profile (`CODESIGNER`) as the leaf profile
   (`FIRMWARE-CODESIGNER`) and set only what differs:
   - Type: **End Entity**.
   - Available key algorithms: **RSA** only. Available bit lengths: **3072** only (the original also allowed 4096).
     Signature algorithm: leave **Inherit from issuing CA** (the sub-CA signs with SHA256WithRSA).
   - Validity: **`2y`** (730 days).
   - Basic Constraints: **Use, Critical** (`CA:FALSE`). Key Usage: **Use, Critical, `Digital Signature` only**.
   - Extended Key Usage: **Use, `Code Signing` only** (1.3.6.1.5.5.7.3.3), not critical, as the Windows profile has it.
   - Subject Key Identifier and Authority Key Identifier: **Use**.
   - CRL Distribution Points, Authority Information Access, OCSP, Subject Alternative Name, Qualified Certificate
     statements: **not used**. All overrides off, as the original has them.
   - Available CAs: **only `Iain Chesworth Labs Firmware CA`** (the original offered another CA only). **Save**.
2. **End entity profile.** **Clone** the code signing end entity profile (`local code signing`) as
   `Firmware CodeSigning` and set:
   - Subject DN attributes: **CN** (required, modifiable) and **O**, added from the list: type `Iain Chesworth Labs`,
     required, **not modifiable**, so that the RA page cannot mistype it. Nothing else: no e-mail, no OU, no
     locality.
   - **End Entity E-mail: untick Use.**
   - Subject Alternative Name: **none**. Subject Directory Attributes: none.
   - Default and Available Certificate Profiles: **the leaf profile** only (click it alone: the clone keeps the old
     one selected).
   - Default and Available CAs: **`Iain Chesworth Labs Firmware CA`** only.
   - Default and Available Tokens: **User Generated** only (click it alone: the clone has P12, BCFKS, JKS and PEM
     selected too). Those let EJBCA make the key, which would then travel; this profile accepts only your request.
   - Key Recoverable: off (it is, unless "Use" is ticked).
3. **Roles.** Your own administrator or RA role must be allowed to use the new CA and the new end entity
   profile (*System Functions, Roles and Access Rules*). A super administrator already is. If the "Make New Request"
   page does not list them, this is why.

## 4. Make each key and its request (on your machine)

Do this twice, once for `stable` and once for `beta`, in a folder **outside every Git checkout and worktree**
(for example `D:\keys\hearth-firmware`). The key never leaves this folder except into the GitHub secret in step 7.
Use a different passphrase for each key and keep both in your password manager.

On Windows, use **PowerShell** with the openssl that Git ships (WSL or Linux works as written, with `openssl` in
place of `& $ossl`):

```powershell
cd D:\keys\hearth-firmware
$ossl = "C:\Program Files\Git\mingw64\bin\openssl.exe"
& $ossl genrsa -aes256 -out stable.key 3072
& $ossl req -new -key stable.key -sha256 -subj "/CN=Hearth Firmware Stable/O=Iain Chesworth Labs" -out stable.csr
& $ossl req -in stable.csr -noout -verify -subject
(Select-String -Path stable.key -Pattern ENCRYPTED).Count
```

The first command asks twice for a passphrase. The last line prints `2` for a protected key (its BEGIN and END
lines); `0` means no passphrase, so delete the file and redo it. The check before it prints `verify OK` and the
subject. `stable.csr` is the only file that goes to EJBCA. For beta: `beta.key`, `beta.csr`, and
`/CN=Hearth Firmware Beta/O=Iain Chesworth Labs`.

**Git Bash goes wrong here in two ways.** openssl's passphrase prompt can be invisible, so the command looks hung (a
`winpty` prefix shows it), and Git Bash rewrites `-subj "/CN=..."` into a Windows path (`C:/Program Files/Git/CN=...`),
even through `winpty` and with `MSYS2_ARG_CONV_EXCL='*'` set. Use PowerShell for these commands.

**The Windows-store route, for completeness.** `certreq -new` makes the key inside the Windows certificate store as
a non-exportable key and writes a request; `certreq -accept` installs the certificate. That key can only sign on
that machine (the signing tool has a PowerShell form for it), so CI could not use it, and **it does not fit how CI
signs Windows today**, which is a PFX in a secret and `signtool` on a Windows runner. Use the key-file route above.

## 5. Issue the two leaves

The firmware CA's crypto token must be active (it is, right after you made it; after an EJBCA restart, activate it
on *Crypto Tokens*). *RA Web* (`/ejbca/ra/`), **Enrollment, Make New Request** (in the older admin web: *RA
Functions, Add End Entity*, then the public web's enrollment). For each leaf:

1. Certificate Type: the end entity profile (`Firmware CodeSigning`). Certificate Subtype: the leaf profile
   (`FIRMWARE-CODESIGNER`). Certification Authority: **`Iain Chesworth Labs Firmware CA`**.
2. Key pair generation: **Provided by user**; upload or paste `stable.csr`. (A certificate request is public.)
3. Subject DN: CN **`Hearth Firmware Stable`**; O is fixed to `Iain Chesworth Labs`. (The names are pinned in the
   firmware; spell them exactly. They are firmware build settings, so changing one later is a build change.)
4. A username (`hearth-firmware-stable`) and an enrollment code (these are EJBCA's, not secrets of the firmware; do
   not screenshot them).
5. **Download PEM** (the leaf alone, not the full chain: you already have `firmware-ca.pem`). Save it beside the key,
   for example as `Hearth Firmware Stable.pem`.
6. Repeat for `Hearth Firmware Beta` with `beta.csr`.

When both are issued, **deactivate the firmware CA's token** if you did not turn auto-activation on: nothing needs the
CA key until a leaf is renewed.

## 6. Check what was issued (public data: paste it back, or give the session the file path)

```bash
openssl x509 -in stable.pem -noout -subject -issuer -dates -fingerprint -sha256 \
    -ext basicConstraints,keyUsage,extendedKeyUsage,subjectKeyIdentifier,authorityKeyIdentifier
openssl verify -partial_chain -purpose codesign -CAfile firmware-ca.pem stable.pem
openssl x509 -in stable.pem -noout -text | grep -E "Public-Key|Signature Algorithm"
openssl x509 -in stable.pem -noout -pubkey | openssl sha256
openssl req -in stable.csr -noout -pubkey | openssl sha256
```

**`-partial_chain` is needed.** A plain `openssl verify -CAfile firmware-ca.pem stable.pem` fails, because OpenSSL
wants a self-signed certificate at the top and the sub-CA is not one; a board treats any certificate in its anchor
list as an anchor, which is what `-partial_chain` asks for. To check the whole path up to your root as well:
`openssl verify -CAfile <root> -untrusted <intermediate> -untrusted firmware-ca.pem stable.pem`. The leaf must **not**
verify with the root and the Intermediate alone (without `-untrusted firmware-ca.pem`).

Expected, for both leaves: subject `CN=Hearth Firmware Stable` (or Beta) with `O=Iain Chesworth Labs`; issuer
`Iain Chesworth Labs Firmware CA`; two years; `CA:FALSE` (critical); `Digital Signature` (critical); `Code Signing`
(not critical) and nothing else; the authority key identifier equal to the sub-CA's subject key identifier; no subject
alternative name, CRL distribution point or authority information access; `Public-Key: (3072 bit)`;
`sha256WithRSAEncryption`; both verifies `OK`; and the two `sha256` lines of the public key equal (the certificate
holds the key you made). Send the outputs, or the file path of the certificate, to the session to check **before
step 7**.

## 7. Put the leaves where CI reads them

A session never creates a secret or the switch below. The environments are different: the owner may ask a session to
create them through the API (as was done on 2026-10-25), or click them in *Settings, Environments*:

| Environment | Rule | Secrets |
|---|---|---|
| `firmware-beta` | Deployment branches and tags: **`main` only**. **No reviewer**: it signs on its own (your decision) | `FIRMWARE_SIGN_PFX_BASE64`, `FIRMWARE_SIGN_PFX_PASSWORD` |
| `firmware-stable` | Deployment branches and tags: **`main` only**. **Required reviewers: you**, so every signing run waits for your approval (your decision). Leave *Prevent self-review* **off** (you start the release, and you are the one who approves its signing). Turn *Allow administrators to bypass configured protection rules* **off**, if the page shows it, so the approval cannot be skipped | the same two names, with the stable leaf's values |

**There is no tag ruleset.** The signing workflow starts when the `Release` workflow has finished (`workflow_run`), and a workflow started that way always runs the copy of the file on `main`; with both environments allowing `main` only, a key can be read only by a workflow file that has been merged, and a tag cannot reach one ([CI signing](esp32-ota.md#ci-signing)). An earlier version of this guide asked for stable on `v*` tags and a `v*` tag ruleset; `release.yml`'s dispatch path pushes the release tag itself with `GITHUB_TOKEN`, which a ruleset that restricts creating `v*` tags blocks (a ruleset's bypass list cannot hold `GITHUB_TOKEN`), and a job it called ran with the ref `main`, which an environment limited to tags refuses. If you set an environment to `v*` already, change it: *Settings, Environments, (the environment), Deployment branches and tags*: remove the `v*` rule and leave `main`.

Make each PFX in PowerShell, from the key folder (it asks for the key's passphrase, then for an **export password**
twice: a long generated one, kept in your password manager):

```powershell
Copy-Item "<where you saved>\firmware-ca.pem" firmware-ca.pem
& $ossl pkcs12 -export -inkey stable.key -in "Hearth Firmware Stable.pem" -certfile firmware-ca.pem -name "Hearth Firmware Stable" -out stable.pfx
& $ossl pkcs12 -in stable.pfx -info -noout
```

The last command asks for the export password and prints only metadata (a `MAC: sha256` line). Then set the secrets
with `gh`, in PowerShell, which keeps the value off the command line (a pipe, and a hidden prompt):

```powershell
[Convert]::ToBase64String([IO.File]::ReadAllBytes("stable.pfx")) | gh secret set FIRMWARE_SIGN_PFX_BASE64 --env firmware-stable --repo iainchesworthlabs/iclforge
gh secret set FIRMWARE_SIGN_PFX_PASSWORD --env firmware-stable --repo iainchesworthlabs/iclforge
```

(The same two lines with `beta.pfx` and `firmware-beta`.) Or paste each value into the secret's box in the browser.
`FIRMWARE_SIGN_PFX_BASE64` is the base64 of the PFX; `FIRMWARE_SIGN_PFX_PASSWORD` is the export password you typed.
**Do not reuse the `WIN_TESTSIGN_PFX_*` organisation secrets**: those are the Windows key, shared with other
repositories, and the firmware keys must stand apart. Read the names back (they never show values):
`gh api repos/iainchesworthlabs/iclforge/environments/firmware-stable/secrets --jq '.secrets[].name'`.

Delete the `.pfx` files once the secrets are saved, and keep `stable.key` and `beta.key` with their passphrases as the
offline copy (two places).

**Last, the switch:** `gh variable set PUBLISH_FIRMWARE_CHANNELS --body true --repo iainchesworthlabs/iclforge`. While it
is not `true` the signing workflow does nothing. Once the workflow is on `main`, the next release signs and publishes
the channels for real; set it to `false` first if you want a `dry_run` dispatch before that.

## 8. What to give back

- `firmware-ca.pem` (the sub-CA's public certificate): it becomes `firmware/keys/trust.pem` in the firmware.
- The step 6 output for both leaves, or the paths of the two certificate files.
- That both environments (`main` only), their four secret names and the variable are set as in step 7.

Rolling a leaf over later: activate the firmware CA's token, make a new key and CSR as step 4, issue it with the same
name as step 5 (the CA does not enforce unique DNs, step 2, so a new end entity with the same name is accepted; or
reuse the old one by setting its status to New), replace the two secrets in that environment, deactivate the token,
and run the signing workflow again for the current release. No board changes.

## As built (2026-10-25)

| | |
|---|---|
| EJBCA | 7.11.0 Community |
| Sub-CA | `Iain Chesworth Labs Firmware CA`: RSA 3072, signed by the owner's Intermediate CA (sha512WithRSA), valid 2026-10-25 to 2034-10-23, `CA:TRUE, pathlen:0`, key usage `keyCertSign` and `cRLSign`; SHA-256 fingerprint in `firmware/keys/README.md` |
| Profiles | certificate profiles `FIRMWARE-SUBCA`, `FIRMWARE-CODESIGNER`; end entity profile `Firmware CodeSigning`; the sub-CA's soft token has auto-activation off and is locked when not issuing |
| `Hearth Firmware Stable` | valid 2026-10-25 to 2028-10-24, SHA-256 `16:75:8E:FD:99:6A:D1:70:0D:5D:02:71:E7:80:C4:86:96:13:79:E5:73:54:39:F4:60:6C:6C:92:19:50:CB:05` |
| `Hearth Firmware Beta` | valid 2026-10-25 to 2028-10-24, SHA-256 `2F:BA:1A:6F:E9:AC:29:24:0D:6B:78:A4:B0:27:F5:5D:0E:EC:87:50:21:06:D6:6D:C7:46:AB:04:E7:82:A5:BB` |
| GitHub | read back from GitHub on 2026-10-25: `firmware-beta` allows `main` only, no reviewer; `firmware-stable` allows `main` only, required reviewer the owner, *Prevent self-review* off, administrators cannot bypass; each has the two secrets; variable `PUBLISH_FIRMWARE_CHANNELS` is `true`; no `v*` tag ruleset (not needed; the repository's rulesets are the merge queue and a disabled code-scanning gate) |

Dates to act on: both leaves end **2028-10-24** (the CI job warns 60 days before, about 2028-08-25), and the sub-CA and
everything under it end **2034-10-23**, so an anchor rollover ([Rollover](esp32-ota.md#rollover-floors-and-revocation))
has to ship in a build well before then.
