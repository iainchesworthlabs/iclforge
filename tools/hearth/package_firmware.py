#!/usr/bin/env python3
"""Package hearth_sink board builds for publishing (planning/esp32-ota.md, O8).

    python tools/hearth/package_firmware.py package --build-dir BUILD \
        --name hearth-sink-esp32s3 --out DIR
    python tools/hearth/package_firmware.py manifest --out DIR

`package` reads one ESP-IDF build directory of firmware/hearth-sink
(its project_description.json, flash_args, sdkconfig and the files flash_args
names) and writes into DIR:

- <name>-<version>.bin: the app image, for an update over the network
  (tools/hearth/ota.py push, the board's page, hearth's Firmware tab);
- <name>-<version>-factory.bin: every region flash_args writes, laid out as
  one file written at 0x0, with 0xFF between regions as erased flash has. It
  is for a new board: it overwrites NVS too;
- <name>-<version>-parts.zip: the same regions as separate files, with a
  flash_args of names relative to the archive. `esptool write-flash
  @flash_args` from the unpacked directory moves a board already in use to
  this layout with its NVS kept. The build's own flash_args names the audio
  partition's source by a path outside the build directory, so it cannot be
  shipped as it is;
- <name>-<version>-elf.zip: the ELF, so a backtrace or a core dump (O4) from
  this image can be read;
- <name>.json: what hearth-sink-manifest.json says about this image.

<version> is the version the image itself carries (esp_app_desc_t), so the
file names and the image agree. An image with a Wi-Fi network built into it
is refused: a published image gets its network from Improv or the board's
page, and keeps it in NVS. So is an image whose screen is served over HTTP
(CONFIG_ICLFORGE_EXAMPLE_PANEL_CAPTURE_PORT other than 0): that server has no
authentication and is a bench's tool.

`manifest` merges every <name>.json in DIR into hearth-sink-manifest.json,
which ota.py push --release reads to choose an image for each board, and
tools/ci/check_firmware_package.py checks before anything is uploaded.

`index` writes the update index of one channel from a release's manifest:
hearth-update-<channel>.json, the small file a board reads to learn what the
newest release is and, for each image, its size and SHA-256 (planning/esp32-ota.md,
O10). sign_index.py signs it afterwards. The facts are the manifest's, so they
have one source; the board picks its image by them.

Stdlib only, as the CI container's Python needs nothing installed for it.
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import re
import struct
import sys
import time
import zipfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any

MANIFEST_NAME = "hearth-sink-manifest.json"
MANIFEST_FORMAT = 1
INDEX_FORMAT = 1
CHANNELS = ("stable", "beta")

# esp_image_header_t (24 bytes), esp_image_segment_header_t (8), then the
# first segment's esp_app_desc_t: ESP-IDF v6.1's esp_app_format.h and
# esp_app_desc.h.
IMAGE_MAGIC = 0xE9
APP_DESC_AT = 32
APP_DESC_MAGIC = 0xABCD5432

# ESP-IDF's targets, their chip names as GET /hardware reports them, and
# esp_chip_id_t, the image header's chip field.
TARGETS = {
    "esp32": ("ESP32", 0x0000),
    "esp32s2": ("ESP32-S2", 0x0002),
    "esp32c3": ("ESP32-C3", 0x0005),
    "esp32s3": ("ESP32-S3", 0x0009),
    "esp32c2": ("ESP32-C2", 0x000C),
    "esp32c6": ("ESP32-C6", 0x000D),
    "esp32h2": ("ESP32-H2", 0x0010),
    "esp32p4": ("ESP32-P4", 0x0012),
    "esp32c61": ("ESP32-C61", 0x0014),
    "esp32c5": ("ESP32-C5", 0x0017),
}

# A partition table entry (esp_partition_info_t): 32 bytes each, until one
# whose magic is not this; an MD5 entry (0xEBEB) or erased flash ends it.
PARTITION_MAGIC = 0x50AA
APP_PARTITION_TYPE = 0x00


class PackageError(Exception):
    """Why a build cannot be packaged; main() prints it."""


@dataclass(frozen=True)
class Region:
    offset: int
    path: Path


def c_string(data: bytes, at: int, size: int) -> str:
    field = data[at : at + size]
    return field.split(b"\0", 1)[0].decode("utf-8", "replace")


def sha256_hex(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def read_flash_args(build: Path) -> tuple[list[str], list[Region]]:
    """flash_args' first line (esptool's options) and its regions, in its order."""
    path = build / "flash_args"
    try:
        lines = [line.strip() for line in path.read_text("utf-8").splitlines() if line.strip()]
    except OSError as error:
        raise PackageError(f"{path} cannot be read: {error}") from error
    if not lines or not lines[0].startswith("--"):
        raise PackageError(f"{path} does not start with esptool's options")
    regions = []
    for line in lines[1:]:
        offset, _, name = line.partition(" ")
        if not offset.startswith("0x") or not name:
            raise PackageError(f"{path}: '{line}' is not '<offset> <file>'")
        regions.append(Region(int(offset, 16), (build / name.strip()).resolve()))
    return lines[0].split(), regions


def read_partitions(table: bytes) -> list[dict[str, Any]]:
    entries = []
    for at in range(0, len(table) - 31, 32):
        magic, ptype, subtype, offset, size = struct.unpack_from("<HBBII", table, at)
        if magic != PARTITION_MAGIC:
            break
        label = c_string(table, at + 12, 16)
        entries.append(
            {"label": label, "type": ptype, "subtype": subtype, "offset": offset, "size": size}
        )
    return entries


def read_sdkconfig(path: Path) -> dict[str, str]:
    values = {}
    for line in path.read_text("utf-8").splitlines():
        match = re.match(r"^(CONFIG_[A-Z0-9_]+)=(.*)$", line.strip())
        if match:
            value = match.group(2)
            if len(value) >= 2 and value[0] == value[-1] == '"':
                value = value[1:-1]
            values[match.group(1)] = value
    return values


def lay_out(regions: list[tuple[int, bytes]]) -> bytes:
    """The regions written into one file from 0x0, with erased flash (0xFF) between them."""
    end = max(offset + len(data) for offset, data in regions)
    flash = bytearray(b"\xff" * end)
    for offset, data in sorted(regions, key=lambda region: region[0]):
        flash[offset : offset + len(data)] = data
    return bytes(flash)


def zip_member_names(regions: list[Region]) -> list[str]:
    """A name for each region's file inside parts.zip: its own, made unique."""
    names: list[str] = []
    for region in regions:
        name = region.path.name
        stem, dot, suffix = name.partition(".")
        candidate = name
        count = 2
        while candidate in names:
            candidate = f"{stem}-{count}{dot}{suffix}"
            count += 1
        names.append(candidate)
    return names


def add_to_zip(archive: zipfile.ZipFile, name: str, data: bytes) -> None:
    """A member with a fixed date, so that one build packages to the same bytes every time."""
    info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
    info.compress_type = zipfile.ZIP_DEFLATED
    info.external_attr = 0o644 << 16
    archive.writestr(info, data)


def file_entry(path: Path) -> dict[str, Any]:
    data = path.read_bytes()
    return {"name": path.name, "size": len(data), "sha256": sha256_hex(data)}


def hold_to_profile(
    build: Path, name: str, ident: str, profiles: Path | None, warn_only: bool = False
) -> None:
    """Refuses a build that is not the profile `ident` (firmware/hearth-sink/profiles.json),
    or whose profile publishes an image of another name. An image published under a board's
    name goes onto that board, so what it was built from is held to the board's profile."""
    # Beside this file, and it imports this one's chip table: imported here, not at the top.
    import sink_profile  # noqa: PLC0415

    try:
        data = sink_profile.load(profiles) if profiles else sink_profile.load()
        spec = sink_profile.profile(data, ident)
        findings = sink_profile.verify(data, ident, build)
    except sink_profile.ProfileError as error:
        raise PackageError(str(error)) from error
    image = spec.get("image")
    if not image or image.get("name") != name:
        raise PackageError(f"profile {ident} does not publish an image named '{name}'")
    if findings:
        shown = "; ".join(f"[{finding.rule}] {finding.message}" for finding in findings[:5])
        more = f" (and {len(findings) - 5} more)" if len(findings) > 5 else ""
        if warn_only:
            print(
                f"package_firmware: warning: the build is not profile {ident}: {shown}{more}",
                file=sys.stderr,
            )
            return
        raise PackageError(f"the build is not profile {ident}: {shown}{more}")


def package(
    build: Path,
    name: str,
    out: Path,
    profile: str | None = None,
    profiles: Path | None = None,
    warn_only: bool = False,
) -> dict[str, Any]:
    """Writes one image's files and its manifest fragment into `out`, and returns the fragment."""
    if profile:
        hold_to_profile(build, name, profile, profiles, warn_only)
    try:
        description = json.loads((build / "project_description.json").read_text("utf-8"))
    except (OSError, ValueError) as error:
        raise PackageError(f"{build} has no readable project_description.json: {error}") from error
    target = str(description.get("target", ""))
    if target not in TARGETS:
        raise PackageError(f"the build's target is '{target}', which this packager does not know")
    chip, chip_id = TARGETS[target]
    config = Path(str(description.get("config_file", "") or build / "sdkconfig"))
    try:
        sdkconfig = read_sdkconfig(config)
    except OSError as error:
        raise PackageError(f"the build's sdkconfig ({config}) cannot be read: {error}") from error
    if sdkconfig.get("CONFIG_ICLFORGE_EXAMPLE_WIFI_SSID") or sdkconfig.get(
        "CONFIG_ICLFORGE_EXAMPLE_WIFI_PASSWORD"
    ):
        raise PackageError(
            "this build has a Wi-Fi network built into its image "
            "(CONFIG_ICLFORGE_EXAMPLE_WIFI_SSID or _PASSWORD); a published image must not"
        )
    capture_port = sdkconfig.get("CONFIG_ICLFORGE_EXAMPLE_PANEL_CAPTURE_PORT", "0")
    if capture_port != "0":
        raise PackageError(
            f"this build serves its screen on port {capture_port} "
            "(CONFIG_ICLFORGE_EXAMPLE_PANEL_CAPTURE_PORT): that HTTP server reads the picture and "
            "presses the buttons for anyone on the network, so a published image must not have it"
        )

    app_path = build / str(description.get("app_bin", ""))
    elf_path = build / str(description.get("app_elf", ""))
    try:
        app = app_path.read_bytes()
    except OSError as error:
        raise PackageError(f"the build's app image ({app_path}) cannot be read: {error}") from error
    if len(app) < APP_DESC_AT + 256 or app[0] != IMAGE_MAGIC:
        raise PackageError(f"{app_path} is not an ESP-IDF application image")
    if struct.unpack_from("<I", app, APP_DESC_AT)[0] != APP_DESC_MAGIC:
        raise PackageError(f"{app_path} has no application description where ESP-IDF puts one")
    if struct.unpack_from("<H", app, 12)[0] != chip_id:
        raise PackageError(f"{app_path} is not an image for the {chip} the build targets")
    version = c_string(app, APP_DESC_AT + 16, 32)
    if not version or not re.fullmatch(r"[A-Za-z0-9._+-]+", version):
        raise PackageError(f"the image's version, '{version}', cannot go into a file name")

    options, regions = read_flash_args(build)
    region_data = []
    for region in regions:
        try:
            region_data.append((region.offset, region.path.read_bytes()))
        except OSError as error:
            raise PackageError(
                f"flash_args names {region.path}, which cannot be read: {error}"
            ) from error
    app_regions = [data for (_, data) in region_data if data == app]
    if len(app_regions) != 1:
        raise PackageError("flash_args does not write the app image exactly once")
    table_regions = [data for (offset, data) in region_data if offset == 0x8000]
    if not table_regions:
        raise PackageError("flash_args writes no partition table at 0x8000")
    partitions = read_partitions(table_regions[0])
    slots = [entry["size"] for entry in partitions if entry["type"] == APP_PARTITION_TYPE]
    if not slots:
        raise PackageError("the partition table has no app slot")

    out.mkdir(parents=True, exist_ok=True)
    stem = f"{name}-{version}"
    app_file = out / f"{stem}.bin"
    app_file.write_bytes(app)
    factory_file = out / f"{stem}-factory.bin"
    factory_file.write_bytes(lay_out(region_data))

    parts_file = out / f"{stem}-parts.zip"
    names = zip_member_names(regions)
    parts = []
    with zipfile.ZipFile(parts_file, "w") as archive:
        lines = [" ".join(options)]
        for member, (offset, data) in zip(names, region_data, strict=True):
            add_to_zip(archive, member, data)
            lines.append(f"{offset:#x} {member}")
            parts.append(
                {"offset": offset, "file": member, "size": len(data), "sha256": sha256_hex(data)}
            )
        add_to_zip(archive, "flash_args", ("\n".join(lines) + "\n").encode("utf-8"))

    elf_file = out / f"{stem}-elf.zip"
    try:
        elf = elf_path.read_bytes()
    except OSError as error:
        raise PackageError(f"the build's ELF ({elf_path}) cannot be read: {error}") from error
    with zipfile.ZipFile(elf_file, "w") as archive:
        add_to_zip(archive, f"{stem}.elf", elf)

    min_rev, max_rev = struct.unpack_from("<HH", app, 15)
    fragment = {
        "name": name,
        "version": version,
        "project": c_string(app, APP_DESC_AT + 48, 32),
        "idf_version": c_string(app, APP_DESC_AT + 112, 32),
        "target": target,
        "chip": chip,
        "chip_id": chip_id,
        "min_rev_full": min_rev,
        "max_rev_full": max_rev,
        "flash_size": sdkconfig.get("CONFIG_ESPTOOLPY_FLASHSIZE", ""),
        "psram": sdkconfig.get("CONFIG_SPIRAM") == "y",
        # esp_app_desc_t's secure_version (CONFIG_BOOTLOADER_APP_SECURE_VERSION): the floor a
        # pull will not go under. ESP-IDF's anti-rollback, which would burn eFuses, stays off.
        "secure_version": struct.unpack_from("<I", app, APP_DESC_AT + 4)[0],
        "slot_bytes": min(slots),
        "partitions": partitions,
        "elf_sha256": app[APP_DESC_AT + 144 : APP_DESC_AT + 176].hex(),
        "network_built_in": False,
        "profile": profile or "",
        "files": {
            "app": file_entry(app_file),
            "factory": file_entry(factory_file),
            "parts": file_entry(parts_file),
            "elf": file_entry(elf_file),
        },
        "parts": parts,
    }
    (out / f"{name}.json").write_text(json.dumps(fragment, indent=2) + "\n", "utf-8")
    return fragment


def merge_manifest(out: Path) -> dict[str, Any]:
    """hearth-sink-manifest.json from every image's fragment in `out`."""
    images = []
    for path in sorted(out.glob("*.json")):
        if path.name == MANIFEST_NAME:
            continue
        images.append(json.loads(path.read_text("utf-8")))
    if not images:
        raise PackageError(f"{out} holds no image's fragment to merge")
    images.sort(key=lambda image: str(image.get("name", "")))
    versions = sorted({image["version"] for image in images})
    manifest = {
        "format": MANIFEST_FORMAT,
        "version": versions[0] if len(versions) == 1 else "",
        "images": images,
    }
    (out / MANIFEST_NAME).write_text(json.dumps(manifest, indent=2) + "\n", "utf-8")
    return manifest


def build_index(
    manifest: dict[str, Any], channel: str, sequence: int, released: str
) -> dict[str, Any]:
    """The update index of `channel` for the release `manifest` describes."""
    if channel not in CHANNELS:
        raise PackageError(f"the channel is '{channel}'; it is one of {', '.join(CHANNELS)}")
    version = str(manifest.get("version", ""))
    if not version:
        raise PackageError(
            "the manifest's images are of more than one version; an index names one release"
        )
    images = []
    for image in manifest.get("images", []):
        app = (image.get("files") or {}).get("app") or {}
        images.append(
            {
                "name": image["name"],
                "file": app["name"],
                "size": app["size"],
                "sha256": app["sha256"],
                "project": image["project"],
                "chip_id": image["chip_id"],
                "min_rev_full": image["min_rev_full"],
                "max_rev_full": image["max_rev_full"],
                "flash_size": image["flash_size"],
                "psram": image["psram"],
                "secure_version": image.get("secure_version", 0),
            }
        )
    if not images:
        raise PackageError("the manifest lists no images")
    return {
        "format": INDEX_FORMAT,
        "channel": channel,
        "sequence": sequence,
        "version": version,
        "released": released,
        "images": images,
    }


def write_index(
    manifest_path: Path,
    channel: str,
    out: Path,
    sequence: int | None = None,
    released: str | None = None,
) -> dict[str, Any]:
    """Writes hearth-update-<channel>.json for the release `manifest_path` describes. The index
    is held to the rules a board reads it by (index_verify.check_index) before it is kept."""
    # Beside this file; imported here so that packaging needs nothing of the signing side.
    import index_verify  # noqa: PLC0415

    try:
        manifest = json.loads(manifest_path.read_text("utf-8"))
    except (OSError, ValueError) as error:
        raise PackageError(f"{manifest_path} cannot be read: {error}") from error
    now = dt.datetime.now(dt.timezone.utc)
    try:
        document = build_index(
            manifest,
            channel,
            sequence if sequence is not None else int(time.time()),
            released or f"{now:%Y-%m-%d}",
        )
    except KeyError as error:
        raise PackageError(f"the manifest's image has no {error}") from error
    data = (json.dumps(document, separators=(",", ":")) + "\n").encode("ascii")
    try:
        index_verify.check_index(data, channel)
    except index_verify.VerifyError as error:
        raise PackageError(f"the index would not be one a board reads: {error.text}") from error
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(data)
    return document


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    commands = parser.add_subparsers(dest="command", required=True)
    one = commands.add_parser("package", help="package one build directory")
    one.add_argument("--build-dir", type=Path, required=True)
    one.add_argument("--name", required=True, help="the image's name, such as hearth-sink-esp32s3")
    one.add_argument("--out", type=Path, required=True)
    one.add_argument(
        "--profile",
        help="the profile of firmware/hearth-sink/profiles.json the build must hold to; "
        "without it the build is packaged as it is",
    )
    one.add_argument(
        "--warn-only",
        action="store_true",
        help="with --profile, say what the build differs in and package it anyway",
    )
    merge = commands.add_parser(
        "manifest", help="merge the fragments in DIR into hearth-sink-manifest.json"
    )
    merge.add_argument("--out", type=Path, required=True)
    index = commands.add_parser(
        "index",
        help="write hearth-update-<channel>.json from a release's hearth-sink-manifest.json",
    )
    index.add_argument("--manifest", type=Path, required=True)
    index.add_argument("--channel", choices=CHANNELS, required=True)
    index.add_argument("--out", type=Path, required=True, help="the index file to write")
    index.add_argument("--sequence", type=int, help="default: the time now, in unix seconds")
    index.add_argument("--released", help="YYYY-MM-DD; default: today (UTC)")
    args = parser.parse_args(argv)
    try:
        if args.command == "package":
            fragment = package(
                args.build_dir, args.name, args.out, args.profile, warn_only=args.warn_only
            )
            print(
                f"{args.name} {fragment['version']}: {len(fragment['parts'])} regions, "
                f"{fragment['files']['app']['size']:,}-byte app, "
                f"smallest slot {fragment['slot_bytes']:,}"
            )
        elif args.command == "index":
            document = write_index(
                args.manifest, args.channel, args.out, args.sequence, args.released
            )
            print(
                f"{args.out.name}: channel {document['channel']}, {document['version']}, "
                f"sequence {document['sequence']}, {len(document['images'])} images"
            )
        else:
            manifest = merge_manifest(args.out)
            print(
                f"{MANIFEST_NAME}: {len(manifest['images'])} images, "
                f"version {manifest['version'] or 'mixed'}"
            )
    except PackageError as error:
        print(f"package_firmware: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
