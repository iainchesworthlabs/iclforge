#!/usr/bin/env python3
"""Writes notices/licences/ESP-IDF-components.txt: the licence texts of what the ESP32 sink images link.

The firmware images (firmware/hearth-sink, built in the espressif/idf container) link ESP-IDF and the
third-party components it carries, and the managed LVGL. Their licences ask that the copyright
notice and the licence text travel with a binary, so Hearth's NOTICES.txt (notices/fragments/
hearth-firmware.txt, which includes this file) carries them. The texts are copied from the
components' own files, not retyped, so this reads an ESP-IDF checkout and the LVGL component:

    python tools/generators/gen_esp_idf_notices.py --idf D:/esp/esp-idf \\
        --lvgl firmware/hearth-sink/managed_components/lvgl__lvgl \\
        --out notices/licences/ESP-IDF-components.txt

Which components are the ones linked comes from `esp-idf-sbom create --rem-unused` of the S3, C6 and
P4 sink builds (2026-10-12): FreeRTOS, lwIP, Mbed TLS and TF-PSA-Crypto, picolibc, HTTP Parser,
argtable3, TLSF, wpa_supplicant, the Xtensa HAL headers (S3), and the Wi-Fi, PHY and coexistence
libraries. When ESP-IDF's version changes, run this again against the new checkout; the version
the file was made from is on its first line, and tools/ci/test_firmware_notices.py fails when that is
not the version `.github/workflows/_build.yml` builds with.

The input is untrusted data: it is only read as text and nothing in it is executed.
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

RULE = "=" * 78


def decode(raw: bytes) -> str:
    """UTF-8, line by line, with a line that is not (a name in a copyright line, in Latin-1) read
    as Latin-1 rather than lost."""
    lines = []
    for line in raw.split(b"\n"):
        try:
            lines.append(line.decode("utf-8"))
        except UnicodeDecodeError:
            lines.append(line.decode("latin-1"))
    return "\n".join(lines)


def read(path: Path) -> str:
    text = decode(path.read_bytes())
    return text.replace("\r\n", "\n").strip("\n") + "\n"


def header_comment(path: Path, first_line_contains: str) -> str:
    """The leading /* ... */ comment of a C file, stars removed, which is where its licence is."""
    text = read(path)
    start = text.index("/*")
    end = text.index("*/", start)
    block = text[start + 2:end]
    lines = []
    for line in block.split("\n"):
        stripped = line.strip()
        if stripped.startswith("*"):
            stripped = stripped[1:]
            if stripped.startswith(" "):
                stripped = stripped[1:]
        lines.append(stripped.rstrip())
    out = "\n".join(lines).strip("\n") + "\n"
    if first_line_contains not in out:
        raise SystemExit(f"{path}: the leading comment does not mention {first_line_contains!r}")
    return out


def picolibc(path: Path) -> str:
    """picolibc's COPYING.picolibc, which is a Debian copyright file for the whole of picolibc and
    newlib, without its file lists: each licence's copyright holders once, then the licence text."""
    text = read(path)
    paragraphs = re.split(r"\n\s*\n", text)
    holders: dict[str, list[str]] = {}
    texts: dict[str, str] = {}
    for paragraph in paragraphs:
        lines = paragraph.split("\n")
        if lines[0].startswith("Files:"):
            copyright_lines: list[str] = []
            licence = ""
            current = None
            for line in lines:
                if line.startswith("Copyright:"):
                    current = line[len("Copyright:"):].strip()
                    copyright_lines.append(current)
                elif line.startswith("License:"):
                    licence = line[len("License:"):].strip()
                    current = None
                elif line.startswith(" ") and current is not None and not line.startswith("Files"):
                    # A continuation of the Copyright field.
                    copyright_lines.append(line.strip())
            if licence:
                bucket = holders.setdefault(licence, [])
                for item in copyright_lines:
                    if item and item not in bucket:
                        bucket.append(item)
        elif lines[0].startswith("License:") and len(lines) > 1:
            name = lines[0][len("License:"):].strip()
            body = "\n".join(line[1:] if line.startswith(" ") else line for line in lines[1:])
            texts[name] = body.replace("\n.\n", "\n\n").strip("\n")
    # What is not code that is linked: a build script and a test's own licence.
    skipped = {"AGPL3-1": "a build script", "GPL2-1": "a test"}

    def natural(name: str):
        return [int(part) if part.isdigit() else part for part in re.split(r"(\d+)", name)]

    out = [
        "picolibc 1.8.10 (the C library of ESP-IDF v6.1) is a set of files from many sources: the",
        "copyright holders and licence of each follow, as picolibc's COPYING.picolibc states them,",
        "without the lists of files. A licence's name is only a label here.",
        "",
    ]
    for name in sorted(texts, key=natural):
        if name in skipped:
            continue
        out.append(f"--- Licence {name} ---")
        out.append("Copyright holders (as picolibc lists them):")
        for item in holders.get(name, []):
            out.append(f"  {item}")
        out.append("")
        out.append(texts[name])
        out.append("")
    return "\n".join(out).rstrip("\n") + "\n"


def mbedtls(path: Path) -> str:
    """Mbed TLS's choice of licences. Its LICENSE file holds both texts; the Apache one is the
    first section of this file already and the GPL one is not the one this firmware is under."""
    text = read(path)
    for needle in ("Apache-2.0", "GPL-2.0-or-later"):
        if needle not in text.split("\n\n\n", 1)[0]:
            raise SystemExit(f"{path}: its opening no longer names {needle}")
    return (
        "Copyright The Mbed TLS Contributors.\n\n"
        "Mbed TLS files are provided under a dual Apache-2.0 OR GPL-2.0-or-later licence: a user\n"
        "chooses which of the two to take the code under. This firmware uses it under the Apache\n"
        "License, Version 2.0, whose text is the first section of this file.\n"
    )


def section(title: str, used_by: str, licence: str, source: str, body: str) -> str:
    return (
        f"{RULE}\n{title}\n{RULE}\n"
        f"Used by: {used_by}\n"
        f"Licence: {licence}\n"
        f"Text copied from: {source}\n\n"
        f"{body.rstrip(chr(10))}\n\n"
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--idf", required=True, type=Path, help="an ESP-IDF checkout")
    parser.add_argument("--lvgl", required=True, type=Path, help="the lvgl/lvgl managed component")
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    idf = args.idf
    comp = idf / "components"
    try:
        version = subprocess.run(["git", "-C", str(idf), "describe", "--tags", "--abbrev=0"],
                                 capture_output=True, text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        version = ""
    if not version:
        raise SystemExit("the ESP-IDF checkout has no tag to name its version by")

    parts: list[str] = []
    parts.append(
        f"ESP-IDF version: {version}\n"
        "Made by tools/generators/gen_esp_idf_notices.py from the files named under each heading.\n\n"
    )

    parts.append(section(
        "ESP-IDF and the Espressif components it links",
        "every sink image; ESP-IDF itself, its Wi-Fi, PHY and coexistence libraries, esp_hosted, mDNS "
        "and the LCD and touch drivers are Espressif's",
        "Apache-2.0",
        "ESP-IDF's LICENSE, which is also the licence file of components/esp_wifi/lib, esp_phy/lib and "
        "esp_coex/lib",
        read(idf / "LICENSE"),
    ))

    parts.append(section(
        "Mbed TLS 4.1.0 and TF-PSA-Crypto 1.1.0",
        "TLS and the cryptography under Sendspin's encryption and OTA in the sink images",
        "Apache-2.0 OR GPL-2.0-or-later; used under Apache-2.0, whose text is the first section",
        "components/mbedtls/mbedtls/LICENSE, which states the dual licence",
        mbedtls(comp / "mbedtls" / "mbedtls" / "LICENSE"),
    ))

    parts.append(section(
        "FreeRTOS kernel 10.5.1",
        "every sink image",
        "MIT, Copyright (C) 2017 Amazon.com, Inc. or its affiliates (ESP-IDF's COPYRIGHT.rst)",
        "components/freertos/FreeRTOS-Kernel/LICENSE.md",
        read(comp / "freertos" / "FreeRTOS-Kernel" / "LICENSE.md"),
    ))

    parts.append(section(
        "lwIP 2.2.0",
        "the TCP/IP stack of every sink image",
        "BSD-3-Clause",
        "components/lwip/lwip/COPYING",
        read(comp / "lwip" / "lwip" / "COPYING"),
    ))

    parts.append(section(
        "wpa_supplicant 2.10",
        "the Wi-Fi supplicant of the ESP32-S3 and ESP32-C6 sink images",
        "BSD-3-Clause",
        "components/wpa_supplicant/README",
        read(comp / "wpa_supplicant" / "README"),
    ))

    parts.append(section(
        "HTTP Parser 2.7.0",
        "the HTTP server and client of every sink image",
        "MIT-style (NGINX and Joyent, Inc. terms)",
        "the leading comment of components/http_parser/http_parser.c",
        header_comment(comp / "http_parser" / "http_parser.c", "Permission is hereby granted"),
    ))

    parts.append(section(
        "argtable3 3.2.2",
        "the console component of the ESP32-P4 sink image",
        "BSD-3-Clause, with the licences of the libraries it bundles",
        "components/console/argtable3/LICENSE",
        read(comp / "console" / "argtable3" / "LICENSE"),
    ))

    tlsf = (comp / "heap" / "tlsf" / "tlsf.c").read_text(encoding="utf-8", errors="replace")
    match = re.search(r"SPDX-FileCopyrightText:\s*([^\n]+)", tlsf)
    licence = re.search(r"SPDX-License-Identifier:\s*([^\n]+)", tlsf)
    if not match or not licence or licence.group(1).strip() != "BSD-3-Clause":
        raise SystemExit("components/heap/tlsf/tlsf.c no longer declares BSD-3-Clause in its header")
    parts.append(section(
        "TLSF allocator",
        "ESP-IDF's heap in every sink image",
        "BSD-3-Clause",
        "the SPDX header of components/heap/tlsf/tlsf.c (the file carries the identifier and the "
        "copyright line; this is the standard text of that licence)",
        f"Copyright (c) {match.group(1).strip()}. All rights reserved.\n\n"
        "Redistribution and use in source and binary forms, with or without modification, are\n"
        "permitted provided that the following conditions are met:\n\n"
        "1. Redistributions of source code must retain the above copyright notice, this list of\n"
        "   conditions and the following disclaimer.\n\n"
        "2. Redistributions in binary form must reproduce the above copyright notice, this list of\n"
        "   conditions and the following disclaimer in the documentation and/or other materials\n"
        "   provided with the distribution.\n\n"
        "3. Neither the name of the copyright holder nor the names of its contributors may be used\n"
        "   to endorse or promote products derived from this software without specific prior\n"
        "   written permission.\n\n"
        "THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS \"AS IS\" AND ANY\n"
        "EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF\n"
        "MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE\n"
        "COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,\n"
        "EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF\n"
        "SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)\n"
        "HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR\n"
        "TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS\n"
        "SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.\n",
    ))

    xtensa = comp / "xtensa" / "esp32s3" / "include" / "xtensa" / "config" / "core-isa.h"
    parts.append(section(
        "Xtensa HAL headers",
        "the ESP32-S3 sink image",
        "MIT, Copyright (c) 1999-2021 Tensilica Inc.",
        "the leading comment of components/xtensa/esp32s3/include/xtensa/config/core-isa.h",
        _xtensa_block(xtensa),
    ))

    parts.append(section(
        "picolibc 1.8.10",
        "the C library of every sink image",
        "BSD-style licences of many holders (below)",
        "components/esp_libc/COPYING.picolibc, without its lists of files",
        picolibc(comp / "esp_libc" / "COPYING.picolibc"),
    ))

    tiny = args.lvgl / "src" / "libs" / "tiny_ttf" / "LICENSE.txt"
    parts.append(section(
        "stb_truetype and stb_rect_pack, in LVGL's tiny_ttf",
        "the ESP32-P4 sink image's screen: it rasterises the TrueType fonts with them",
        "MIT (alternative A of two; the other is the public domain)",
        "lvgl/src/libs/tiny_ttf/LICENSE.txt",
        read(tiny),
    ))

    fonts = args.lvgl / "scripts" / "built_in_font" / "font_license"
    parts.append(section(
        "Montserrat, in LVGL's built-in font",
        "the ESP32-P4 sink image: LVGL's default font (lv_font_montserrat_14) is linked in",
        "SIL Open Font License 1.1, Copyright 2011 The Montserrat Project Authors",
        "lvgl/scripts/built_in_font/font_license/Montserrat/OFL.txt",
        read(fonts / "Montserrat" / "OFL.txt"),
    ))
    parts.append(section(
        "Font Awesome 5 Free, the symbols in LVGL's built-in font",
        "the ESP32-P4 sink image: lv_font_montserrat_14 carries LVGL's symbol glyphs",
        "icons CC BY 4.0, fonts SIL OFL 1.1, code MIT (Fonticons, Inc.)",
        "lvgl/scripts/built_in_font/font_license/FontAwesome5/LICENSE.txt",
        read(fonts / "FontAwesome5" / "LICENSE.txt"),
    ))

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text("".join(parts).rstrip("\n") + "\n", encoding="utf-8", newline="\n")
    print(f"wrote {args.out}: {args.out.stat().st_size} bytes, ESP-IDF {version}")
    return 0


def _xtensa_block(path: Path) -> str:
    """The licence paragraph in the Xtensa core-isa.h's comment, after its Customer ID line."""
    text = read(path)
    start = text.index("Permission is hereby granted")
    end = text.index("SOFTWARE.", start) + len("SOFTWARE.")
    lines = [line.strip() for line in text[start:end].split("\n")]
    body = "\n".join(lines)
    return "Copyright (c) 1999-2021 Tensilica Inc.\n\n" + body + "\n"


if __name__ == "__main__":
    sys.exit(main())
