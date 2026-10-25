# Issuing the firmware signing certificates in EJBCA

Written 2026-10-25 for the owner, for [the signed update index of O10](esp32-ota.md#trust-and-signing). It is a
step-by-step for EJBCA's web pages, with the exact fields. It says what to set and why, in the order to do it.
Nothing in it needs a key to be shown to anyone: **never paste, send or screenshot a private key, a PFX, a
password, an enrollment code or a crypto token's activation code**, in a chat or anywhere. Public certificates
are fine to share.

What this makes: **one firmware sub-CA** and **two leaf certificates** under it, `Hearth Firmware Stable` and
`Hearth Firmware Beta`. CI signs with the two leaves (one in each GitHub environment). The board holds the
sub-CA's public certificate, so a Windows signing certificate, or any other certificate your root ever issued,
cannot sign firmware.

## What this is based on

- The Windows test-signing certificate (the shared actions repository `icl-common-infra`: its
  test-signing page and its `issue-leaf.sh` script) is: `CA:FALSE`, key usage `digitalSignature`,
  extended key usage `codeSigning` (1.3.6.1.5.5.7.3.3), RSA 3072, SHA-256, at most 365 days, its common name
  "Iain Chesworth Labs Test Signing", kept as a PFX in organisation secrets and used by CI with `signtool`. The
  firmware leaves are the same kind of certificate, so your existing profile pair `code signer` and
  `local code signing` already describes them. **That document describes an openssl script as the issuer, and you
  say it is EJBCA now; the screenshots below settle which.**
- The board checks: the chain ends at the sub-CA it holds; the leaf is for code signing; the leaf's common name is
  the pinned one for the channel; the signature. It checks no CRL, no OCSP and no timestamp.

## 0. Decisions in this guide

| Question | Answer | Why |
|---|---|---|
| Sub-CA, or the root with the common name pinned? | **A sub-CA** | Whatever else the root ever issued (the Windows certificate, one made by mistake) then cannot sign firmware, and the pin is a second lock instead of the only one. EJBCA, Community and Enterprise, can make sub-CAs. If your root's key is not held by EJBCA (an imported or offline root), use step 2's external route; if even that is more than you want today, the root with a pin works and the board needs no change |
| Key type | **RSA 3072, SHA-256** (the same as the Windows leaf) | The profile pair you have already issues exactly this, so nothing in EJBCA needs a new algorithm enabled. A board already carries RSA for TLS (GitHub's CDN uses an RSA certificate). ECDSA P-256 would make the chain about 2 KB smaller; choose it only if your profile already lists it, since a board handles either |
| Signature scheme | RSA PKCS#1 v1.5 with SHA-256 | What every tool produces without options. The tool also supports PSS if you ever prefer it |
| Validity | **Two years** for the leaves, ten for the sub-CA | A board judges a leaf's dates only when its clock is set. The CI job warns 60 days before a leaf ends. Renewing is a new leaf with the same name; no board changes |
| Extensions | Key usage `digitalSignature` only; extended key usage `codeSigning` only; basic constraints `CA:false`; both critical | As the Windows leaf; nothing else can use the key |
| CRL, OCSP, AIA | **Leave out** of the firmware profiles | The board checks none of them, and each one adds bytes and a URL to every signature file |
| Who makes the key | **You, on your machine, with a certificate request (CSR)** | The private key never reaches EJBCA. The only place it travels to is the GitHub environment secret, which is where CI has to read it |
| Separate profiles | **Copies** of `code signer` and `local code signing`, named `firmware code signer` and `firmware code signing` | Your Windows profiles stay as they are, and these can be restricted to the one sub-CA |

## 1. Look at what you have (screenshots, before changing anything)

Send these to the session, so that it can check them against this guide before anything is issued. Each is a
page of settings, not a secret. Crop out anything that shows a key, a password or a code.

1. **The version.** The footer or the About page of the admin web, showing the EJBCA name and version.
2. **CA Functions, Certification Authorities.** The list: names, status, and subject DNs.
3. **Your root CA, viewed** (its name, then "View" or "Edit" without changing it): subject DN, signing algorithm,
   key specification, validity, "signed by", and whether it is online (its crypto token is active).
4. **CA Functions, Certificate Profiles:** the list, then the `code signer` profile opened with "Edit" (do not
   save): every field from the top to the end, in as many screenshots as it takes. The ones that matter: Type;
   Available Key Algorithms; Available Bit Lengths; Available ECDSA curves; Signature Algorithm; Validity or
   Expiration Restrictions; Basic Constraints; Key Usage; Extended Key Usage; CRL Distribution Points; Authority
   Information Access; Available CAs; Published Publishers.
5. **RA Functions, End Entity Profiles:** `local code signing` opened with "Edit" (do not save), from the top: Subject
   DN Attributes; Subject Alternative Name; Main Certificate Data (Default Certificate Profile, Available
   Certificate Profiles, Default CA, Available CAs, Default Token, Available Tokens); the Other Data section.
   Hide a username or enrollment code if one is shown.
6. **The Windows certificate, as issued:** the output of `openssl x509 -in <that certificate> -noout -text` (it is a
   public certificate), or its Details tab in Windows. The point is its issuer, key size, extensions and validity.

## 2. Make the firmware sub-CA

*CA Functions, Certification Authorities.* Menu names differ a little between EJBCA 7, 8 and 9; if a field below is
not where it says, send a screenshot of the page.

1. **A certificate profile for the sub-CA.** *CA Functions, Certificate Profiles*: clone the profile your root's
   sub-CAs use (`SUBCA` is in the default set) and name the copy `firmware sub CA`. In it:
   - Type: **Sub CA**.
   - Available key algorithms: **RSA**, bit lengths **3072**; signature algorithm **SHA256WithRSA**.
   - Validity: **3650 days** (or `10y`), no longer than the root has left.
   - Basic Constraints: **Use, Critical, CA with Path Length Constraint = 0**. That stops the sub-CA from making
     another CA.
   - Key Usage: **Use, Critical, `Key Cert Sign` and `CRL Sign` only**.
   - Extended Key Usage: leave unset (the board does not read it on a CA).
   - CRL Distribution Points, Authority Information Access, OCSP: not used.
   - Use Certificate Policies, Name Constraints: not used.
2. **Add the CA.** *Certification Authorities*, name `Iain Chesworth Labs Firmware CA`, then **Create**. Fill in:
   - Subject DN: `CN=Iain Chesworth Labs Firmware CA,O=Iain Chesworth Labs`. Match the root's other attributes if it
     has them (country).
   - CA type: X.509. Certificate profile: **`firmware sub CA`**.
   - Crypto token: a new **soft** token, with the key **RSA 3072** for signing and for certificate signing (a
     default and test key alias are fine). If you let EJBCA auto-activate it, say so now; otherwise remember to
     activate it before issuing a leaf, and **never put its activation code in a screenshot**.
   - Signing algorithm: **SHA256WithRSA**.
   - **Signed by**: your root CA (if its key is active in this EJBCA). If the root is offline or lives elsewhere,
     choose **External CA** instead: EJBCA gives you a request (CSR) for the sub-CA, you sign it with your root
     wherever the root key is, with the same profile settings (CA, path length 0, key usage keyCertSign and
     cRLSign, 10 years), and import the signed certificate with the chain.
   - Validity of the CA: as the profile. Description: "Signs Hearth firmware update leaves only".
3. **Download the sub-CA certificate** (*Certification Authorities*, the CA, **Download certificate**, PEM). This
   file is public; it is the anchor in the board's firmware. Send it to the session, or keep it as
   `firmware-ca.pem`.
4. **Check it** (a public certificate, safe to run and to paste): `openssl x509 -in firmware-ca.pem -noout -subject
   -issuer -dates -ext basicConstraints,keyUsage`. You want `CA:TRUE, pathlen:0`, `Certificate Sign, CRL Sign`, and
   dates ten years long. Then `openssl verify -CAfile <your root certificate> firmware-ca.pem` prints `OK`.

## 3. Make the two profiles for the leaves

1. **Certificate profile.** Clone `code signer` as **`firmware code signer`** and set:
   - Type: **End Entity**.
   - Available key algorithms: **RSA**, bit length **3072** only. (If your `code signer` allows ECDSA, untick it:
     the pinned shape is one shape.) Signature algorithm: **SHA256WithRSA**.
   - Validity: **730 days** (`2y`).
   - Basic Constraints: **Use, Critical, CA unticked** (`CA:FALSE`).
   - Key Usage: **Use, Critical, `Digital Signature` only**. No Non Repudiation, no Key Encipherment.
   - Extended Key Usage: **Use, Critical, `Code Signing` only** (1.3.6.1.5.5.7.3.3).
   - Subject Key Identifier and Authority Key Identifier: **Use** (as the Windows leaf).
   - CRL Distribution Points, Authority Information Access, OCSP: **not used**. No Subject Alternative Name
     extension. No Certificate Transparency. No Qualified Certificate statements.
   - Available CAs: **only `Iain Chesworth Labs Firmware CA`**.
2. **End entity profile.** Clone `local code signing` as **`firmware code signing`** and set:
   - Subject DN attributes: **CN** (required, modifiable) and **O** (required, modifiable). Nothing else: no
     e-mail, no OU, no locality.
   - Subject Alternative Name: **none**. Subject Directory Attributes: none.
   - Default and Available Certificate Profiles: **`firmware code signer`** only.
   - Default and Available CAs: **`Iain Chesworth Labs Firmware CA`** only.
   - Default and Available Tokens: **User Generated** only (the key comes from your CSR). If you leave PKCS#12
     tokens on, EJBCA could make the key for you and it would travel; keep it off for this profile.
   - Number of allowed requests: 1 is plenty; Key Recoverable: **off**.
3. **Roles.** Your own administrator or RA role must be allowed to use the new CA and the new end entity
   profile (*System Functions, Roles and Access Rules*). If the "Make New Request" page does not list them, this
   is why.

## 4. Make each key and its request (on your machine, in Git Bash or WSL)

Do this twice, once for `stable` and once for `beta`, in a folder **outside every Git checkout and worktree**
(for example `D:\keys\hearth-firmware`). The key never leaves this folder except into the GitHub secret in step 7.

```bash
openssl genrsa -aes256 -out stable.key 3072
openssl req -new -key stable.key -sha256 \
    -subj "/CN=Hearth Firmware Stable/O=Iain Chesworth Labs" -out stable.csr
```

It asks for a passphrase for the key; choose one and keep it in your password manager. `stable.csr` is the only file
that goes to EJBCA. For beta: `beta.key`, `beta.csr`, and `/CN=Hearth Firmware Beta/O=Iain Chesworth Labs`.

**The Windows-store route, for completeness.** `certreq -new` makes the key inside the Windows certificate store as
a non-exportable key and writes a request; `certreq -accept` installs the certificate. That key can only sign on
that machine (the signing tool has a PowerShell form for it), so CI could not use it, and **it does not fit how CI
signs Windows today**, which is a PFX in a secret and `signtool` on a Windows runner. Use the key-file route above.

## 5. Issue the two leaves

*RA Web* (`/ejbca/ra/`), **Enrollment, Make New Request** (in the older admin web: *RA Functions, Add End Entity*,
then the public web's enrollment). For each leaf:

1. Certificate Type: **`firmware code signing`**. Certificate Subtype: **`firmware code signer`**. Certification
   Authority: **`Iain Chesworth Labs Firmware CA`**.
2. Key pair generation: **Provided by user**; upload or paste `stable.csr`.
3. Subject DN: CN **`Hearth Firmware Stable`**, O **`Iain Chesworth Labs`**. (The names are pinned in the firmware;
   spell them exactly. They are firmware build settings, so changing one later is a build change.)
4. A username and an enrollment code if the page asks (these are EJBCA's, not secrets of the firmware; do not
   screenshot them).
5. **Download** the certificate in PEM, and the chain if offered (`stable.crt`, and `firmware-ca.pem` which you have).
6. Repeat for `Hearth Firmware Beta`.

## 6. Check what was issued (public data: paste it back)

```bash
openssl x509 -in stable.crt -noout -subject -issuer -dates -fingerprint -sha256 \
    -ext basicConstraints,keyUsage,extendedKeyUsage,subjectKeyIdentifier,authorityKeyIdentifier
openssl verify -CAfile firmware-ca.pem stable.crt
openssl x509 -in stable.crt -noout -text | grep -E "Public-Key|Signature Algorithm"
```

Expected, for both leaves: subject `CN=Hearth Firmware Stable` (or Beta); issuer `Iain Chesworth Labs Firmware CA`;
two years; `CA:FALSE` (critical); `Digital Signature` (critical); `Code Signing` (critical) and nothing else; no
subject alternative name; no CRL distribution point; `Public-Key: (3072 bit)`; `sha256WithRSAEncryption`;
`stable.crt: OK`. Send the three outputs for each (certificate text only) to the session to check **before step 7**.

## 7. Put the leaves where CI reads them (you, in the GitHub web pages)

A session never creates a secret. In the repository's *Settings, Environments*:

| Environment | Rule | Secrets |
|---|---|---|
| `firmware-beta` | Deployment branches and tags: `main` and `v*`. **No reviewer**: it signs on its own (your decision) | `FIRMWARE_SIGN_PFX_BASE64`, `FIRMWARE_SIGN_PFX_PASSWORD` |
| `firmware-stable` | Deployment branches and tags: **`v*` only**, not `main`. **Required reviewers: you**, so every signing run waits for your approval (your decision). Leave *Prevent self-review* **off** (you start the run by cutting the tag, and you are the one who approves it). Turn *Allow administrators to bypass configured protection rules* **off**, if the page shows it, so the approval cannot be skipped | the same two names, with the stable leaf's values |

Also add the tag ruleset for `v*` (*Settings, Rules*) so that only you can create a tag that matches: an environment
secret can be read by a run on any ref the environment allows, and for stable that is a tag. The reasons are in
[CI signing](esp32-ota.md#ci-signing).

Make each value this way, then paste it into the secret's box in the browser (do not use a command that puts a key
on a command line):

```bash
openssl pkcs12 -export -inkey stable.key -in stable.crt -certfile firmware-ca.pem \
    -name "Hearth Firmware Stable" -out stable.pfx
base64 -w0 stable.pfx > stable.pfx.b64
```

`FIRMWARE_SIGN_PFX_BASE64` is the content of `stable.pfx.b64`; `FIRMWARE_SIGN_PFX_PASSWORD` is the export password
you typed. **Do not reuse the `WIN_TESTSIGN_PFX_*` organisation secrets**: those are the Windows key, shared with
other repositories, and the firmware keys must stand apart. Keep `stable.key` and `beta.key` and their passphrases
as the offline copy (two places), and delete the `.pfx` and `.b64` files once the secrets are saved.

## 8. What to give back

- `firmware-ca.pem` (the sub-CA's public certificate): it becomes `firmware/keys/trust.pem` in the firmware.
- The step 6 output for both leaves.
- That both environments, the tag ruleset and the two switches above are set as in step 7's table (a screenshot of
  each environment's page, without the secrets' values, will do).

Rolling a leaf over later: make a new key and CSR as step 4, issue it with the same name as step 5, replace the two
secrets in that environment, and run the signing workflow again for the current release. No board changes.
