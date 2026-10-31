# Firmware update trust anchor

`trust.pem` is the public certificate of the firmware signing CA, "Iain Chesworth Labs Firmware CA". It is the
one trust anchor that a Hearth board built from this repository holds for signed update indexes, and the signing
tool checks what CI signs against it. It is public data. No key, PFX, password or code goes in this directory.

| | |
|---|---|
| Subject | `CN=Iain Chesworth Labs Firmware CA, O=Iain Chesworth Labs` |
| Issued by | the owner's local Intermediate CA (`intermediate-ca.90-highett-road.com`) |
| Key | RSA 3072; signed with sha512WithRSAEncryption by the issuer |
| Valid | 2026-10-25 to 2034-10-23 |
| Constraints | `CA:TRUE`, path length 0 (it can sign leaves and no further CA); key usage `keyCertSign` and `cRLSign` |
| SHA-256 fingerprint | `45:0D:81:C7:C4:D7:04:FD:0E:A1:68:1F:AB:94:3A:25:65:D4:0E:17:60:D0:B4:1A:1B:2F:77:09:CA:98:0E:0E` |

To check the file against that fingerprint:

```bash
openssl x509 -in firmware/keys/trust.pem -noout -subject -issuer -dates -fingerprint -sha256 \
    -ext basicConstraints,keyUsage
```

A board does not judge the anchor's own dates ([the design](../../planning/esp32-ota.md#trust-and-signing)), so
2034 is not a deadline for boards in the field. It does bound the leaves, which cannot outlive their CA.

The two leaves that chain to this CA, `Hearth Firmware Stable` and `Hearth Firmware Beta`, are made and renewed
as [the EJBCA guide](../../planning/hearth-firmware-signing-ejbca.md) describes. Changing this file changes what
new builds trust, so a change here is a change to review.
