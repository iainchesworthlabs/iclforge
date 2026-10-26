"""Unit tests for package_firmware.py, which packages hearth_sink board builds
for publishing (planning/esp32-ota.md, O8).

stdlib unittest, as ci.yml's script-lint job runs it. The builds are made from
nothing by sink_build_fixture.py, with an app image the bootloader's checks
would pass.

Run: python3 -m unittest discover -s tools/hearth -p 'test_*.py'
"""

from __future__ import annotations

import hashlib
import io
import json
import sys
import tempfile
import unittest
import zipfile
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import index_verify
import package_firmware as packager
import sink_build_fixture as fixture
import sink_profile


class Package(unittest.TestCase):
    def setUp(self) -> None:
        self.dir = tempfile.TemporaryDirectory()
        self.root = Path(self.dir.name)
        self.out = self.root / "out"

    def tearDown(self) -> None:
        self.dir.cleanup()

    def test_a_build_becomes_the_five_files(self) -> None:
        build = fixture.make_build(self.root / "s3")
        fragment = packager.package(build, "hearth-sink-esp32s3", self.out)
        self.assertEqual(
            sorted(path.name for path in self.out.iterdir()),
            [
                "hearth-sink-esp32s3-v0.11.0-elf.zip",
                "hearth-sink-esp32s3-v0.11.0-factory.bin",
                "hearth-sink-esp32s3-v0.11.0-parts.zip",
                "hearth-sink-esp32s3-v0.11.0.bin",
                "hearth-sink-esp32s3.json",
            ],
        )
        app = (build / "iclforge_hearth_sink.bin").read_bytes()
        self.assertEqual((self.out / "hearth-sink-esp32s3-v0.11.0.bin").read_bytes(), app)
        self.assertEqual(fragment["version"], "v0.11.0")
        self.assertEqual(fragment["target"], "esp32s3")
        self.assertEqual(fragment["chip"], "ESP32-S3")
        self.assertEqual(fragment["chip_id"], 9)
        self.assertEqual((fragment["min_rev_full"], fragment["max_rev_full"]), (0, 99))
        self.assertEqual(fragment["flash_size"], "16MB")
        self.assertTrue(fragment["psram"])
        self.assertEqual(fragment["slot_bytes"], 0x400000)
        self.assertFalse(fragment["network_built_in"])
        self.assertEqual(fragment["project"], "iclforge_hearth_sink")
        self.assertEqual(fragment["elf_sha256"], bytes((1 + i) & 0xFF for i in range(32)).hex())
        self.assertEqual(
            [entry["label"] for entry in fragment["partitions"]][3:5], ["ota_0", "ota_1"]
        )
        self.assertEqual(
            [(part["offset"], part["file"]) for part in fragment["parts"]],
            [
                (0x0, "bootloader.bin"),
                (0x8000, "partition-table.bin"),
                (0x10000, "ota_data_initial.bin"),
                (0x20000, "iclforge_hearth_sink.bin"),
                (0x830000, "sample.ac3"),
                (0x870000, "storage.bin"),
            ],
        )
        self.assertEqual(
            json.loads((self.out / "hearth-sink-esp32s3.json").read_text("utf-8")), fragment
        )
        with zipfile.ZipFile(self.out / "hearth-sink-esp32s3-v0.11.0-elf.zip") as archive:
            self.assertEqual(archive.namelist(), ["hearth-sink-esp32s3-v0.11.0.elf"])

    def test_the_parts_archive_flashes_with_paths_of_its_own(self) -> None:
        build = fixture.make_build(self.root / "p4", target="esp32p4", min_rev=100, max_rev=199)
        packager.package(build, "hearth-sink-esp32p4-rev1", self.out)
        with zipfile.ZipFile(self.out / "hearth-sink-esp32p4-rev1-v0.11.0-parts.zip") as archive:
            flash_args = archive.read("flash_args").decode().splitlines()
            self.assertEqual(flash_args[0], "--flash-mode dio --flash-freq 80m --flash-size 16MB")
            # The P4's bootloader at 0x2000, and the audio source by its own name.
            self.assertEqual(flash_args[1], "0x2000 bootloader.bin")
            self.assertEqual(flash_args[5], "0x830000 sample.ac3")
            self.assertEqual(
                archive.read("sample.ac3"),
                (self.root / "p4" / "stream" / "sample.ac3").read_bytes(),
            )
            for line in flash_args[1:]:
                self.assertIn(line.split()[1], archive.namelist())

    def test_the_factory_image_is_every_region_at_its_offset_over_erased_flash(self) -> None:
        build = fixture.make_build(
            self.root / "c6", target="esp32c6", flash_size="16MB", psram=False
        )
        packager.package(build, "hearth-sink-esp32c6-16mb", self.out)
        factory = (self.out / "hearth-sink-esp32c6-16mb-v0.11.0-factory.bin").read_bytes()
        app = (build / "iclforge_hearth_sink.bin").read_bytes()
        storage = (build / "storage.bin").read_bytes()
        self.assertEqual(len(factory), 0x870000 + len(storage))
        self.assertEqual(factory[0x20000 : 0x20000 + len(app)], app)
        self.assertEqual(factory[0x870000:], storage)
        # Between the bootloader and the partition table, and after the app:
        # erased flash, not zeros.
        self.assertEqual(set(factory[0x2000:0x8000]), {0xFF})
        self.assertEqual(set(factory[0x20000 + len(app) : 0x30000]), {0xFF})

    def test_a_table_with_no_audio_or_storage_ends_its_factory_image_with_the_app(self) -> None:
        # The 4 MB C6's table (partitions_c6.csv) has neither, so its flash_args writes neither.
        build = fixture.make_build(
            self.root / "c6", target="esp32c6", flash_size="4MB", psram=False
        )
        packager.package(build, "hearth-sink-esp32c6", self.out)
        factory = (self.out / "hearth-sink-esp32c6-v0.11.0-factory.bin").read_bytes()
        app = (build / "iclforge_hearth_sink.bin").read_bytes()
        self.assertEqual(len(factory), 0x20000 + len(app))
        self.assertEqual(factory[0x20000:], app)

    def test_packaging_one_build_twice_gives_the_same_bytes(self) -> None:
        build = fixture.make_build(self.root / "s3")
        first = packager.package(build, "hearth-sink-esp32s3", self.root / "one")
        second = packager.package(build, "hearth-sink-esp32s3", self.root / "two")
        self.assertEqual(first["files"], second["files"])

    def test_a_build_with_a_network_built_in_is_refused(self) -> None:
        build = fixture.make_build(self.root / "s3", wifi_ssid="home")
        with self.assertRaisesRegex(packager.PackageError, "Wi-Fi network built into its image"):
            packager.package(build, "hearth-sink-esp32s3", self.out)
        self.assertFalse(self.out.exists())

    def test_a_build_that_serves_its_screen_over_http_is_refused(self) -> None:
        build = fixture.make_build(
            self.root / "p4", target="esp32p4", min_rev=301, max_rev=399, capture_port="8081"
        )
        with self.assertRaisesRegex(packager.PackageError, "serves its screen on port 8081"):
            packager.package(build, "hearth-sink-esp32p4-rev3", self.out)
        self.assertFalse(self.out.exists())

    def test_a_build_with_the_screen_server_off_is_packaged(self) -> None:
        build = fixture.make_build(
            self.root / "p4", target="esp32p4", min_rev=301, max_rev=399, capture_port="0"
        )
        fragment = packager.package(build, "hearth-sink-esp32p4-rev3", self.out)
        self.assertEqual(fragment["name"], "hearth-sink-esp32p4-rev3")

    def test_an_image_for_another_chip_than_the_build_is_refused(self) -> None:
        app = fixture.app_image(fixture.CHIP_IDS["esp32c6"], "v0.11.0")
        build = fixture.make_build(self.root / "s3", app=app)
        with self.assertRaisesRegex(packager.PackageError, "not an image for the ESP32-S3"):
            packager.package(build, "hearth-sink-esp32s3", self.out)

    def test_a_flash_args_that_does_not_write_the_app_is_refused(self) -> None:
        build = fixture.make_build(self.root / "s3")
        args = (
            (build / "flash_args")
            .read_text("utf-8")
            .replace("0x20000 iclforge_hearth_sink.bin\n", "")
        )
        (build / "flash_args").write_text(args, "utf-8")
        with self.assertRaisesRegex(
            packager.PackageError, "does not write the app image exactly once"
        ):
            packager.package(build, "hearth-sink-esp32s3", self.out)

    def test_the_manifest_merges_every_fragment(self) -> None:
        for name, settings in fixture.RELEASE_IMAGES.items():
            build = fixture.make_build(self.root / name, **settings)
            packager.package(build, name, self.out)
        manifest = packager.merge_manifest(self.out)
        self.assertEqual(manifest["format"], 1)
        self.assertEqual(manifest["version"], "v0.11.0")
        self.assertEqual(
            [image["name"] for image in manifest["images"]], sorted(fixture.RELEASE_IMAGES)
        )
        written = json.loads((self.out / packager.MANIFEST_NAME).read_text("utf-8"))
        self.assertEqual(written, manifest)
        # Merged again, the manifest is not read as an image of its own.
        self.assertEqual(len(packager.merge_manifest(self.out)["images"]), 5)

    def test_main_reports_a_refusal_on_stderr_and_exits_1(self) -> None:
        build = fixture.make_build(self.root / "s3", wifi_ssid="home")
        stderr = io.StringIO()
        with redirect_stderr(stderr):
            code = packager.main(
                [
                    "package",
                    "--build-dir",
                    str(build),
                    "--name",
                    "hearth-sink-esp32s3",
                    "--out",
                    str(self.out),
                ]
            )
        self.assertEqual(code, 1)
        self.assertIn(
            "package_firmware: this build has a Wi-Fi network built into its image",
            stderr.getvalue(),
        )


class UpdateIndex(unittest.TestCase):
    """index: the small file a board reads to learn what the newest release is (O10)."""

    def setUp(self) -> None:
        self.dir = tempfile.TemporaryDirectory()
        self.root = Path(self.dir.name)
        self.out = self.root / "out"
        for name, settings in fixture.RELEASE_IMAGES.items():
            secure = 3 if name.endswith("rev3") else 0
            build = fixture.make_build(self.root / name, **settings, secure_version=secure)
            packager.package(build, name, self.out)
        self.manifest = self.out / packager.MANIFEST_NAME
        packager.merge_manifest(self.out)

    def tearDown(self) -> None:
        self.dir.cleanup()

    def write(self, channel: str = "beta", **kwargs) -> dict:
        return packager.write_index(self.manifest, channel, self.root / "index.json", **kwargs)

    def test_the_index_names_each_image_by_the_facts_a_board_matches_on(self) -> None:
        document = self.write(sequence=1793000000, released="2026-11-05")
        self.assertEqual(
            {
                key: document[key]
                for key in ("format", "channel", "sequence", "version", "released")
            },
            {
                "format": 1,
                "channel": "beta",
                "sequence": 1793000000,
                "version": "v0.11.0",
                "released": "2026-11-05",
            },
        )
        by_name = {image["name"]: image for image in document["images"]}
        self.assertEqual(sorted(by_name), sorted(fixture.RELEASE_IMAGES))
        s3 = by_name["hearth-sink-esp32s3"]
        self.assertEqual(s3["file"], "hearth-sink-esp32s3-v0.11.0.bin")
        self.assertEqual(s3["chip_id"], 9)
        self.assertEqual((s3["min_rev_full"], s3["max_rev_full"]), (0, 99))
        self.assertEqual((s3["flash_size"], s3["psram"], s3["secure_version"]), ("16MB", True, 0))
        self.assertEqual(by_name["hearth-sink-esp32p4-rev3"]["secure_version"], 3)
        # Size and SHA-256 are the app image file's own, which the manifest already holds.
        app = (self.out / s3["file"]).read_bytes()
        self.assertEqual(s3["size"], len(app))
        self.assertEqual(s3["sha256"], hashlib.sha256(app).hexdigest())
        self.assertEqual(by_name["hearth-sink-esp32c6"]["flash_size"], "4MB")
        self.assertFalse(by_name["hearth-sink-esp32c6"]["psram"])

    def test_the_file_is_compact_deterministic_and_small_enough_for_a_board(self) -> None:
        self.write(sequence=7, released="2026-11-05")
        first = (self.root / "index.json").read_bytes()
        self.write(sequence=7, released="2026-11-05")
        self.assertEqual((self.root / "index.json").read_bytes(), first)
        self.assertTrue(first.endswith(b"}" + bytes([10])))
        self.assertLess(len(first), 4096)

    def test_the_index_is_held_to_the_rules_a_board_reads_it_by(self) -> None:
        self.write()
        index_verify.check_index((self.root / "index.json").read_bytes(), "beta")

    def test_the_stable_channel_is_its_own_file_content(self) -> None:
        self.assertEqual(self.write("stable")["channel"], "stable")

    def test_a_manifest_of_mixed_versions_has_no_index(self) -> None:
        manifest = json.loads(self.manifest.read_text("utf-8"))
        manifest["version"] = ""
        self.manifest.write_text(json.dumps(manifest), "utf-8")
        with self.assertRaises(packager.PackageError):
            self.write()

    def test_an_image_whose_file_name_a_board_would_refuse_has_no_index(self) -> None:
        manifest = json.loads(self.manifest.read_text("utf-8"))
        manifest["images"][0]["files"]["app"]["name"] = "../evil.bin"
        self.manifest.write_text(json.dumps(manifest), "utf-8")
        with self.assertRaises(packager.PackageError):
            self.write()
        self.assertFalse((self.root / "index.json").exists())

    def test_a_manifest_older_than_secure_version_gets_zero(self) -> None:
        manifest = json.loads(self.manifest.read_text("utf-8"))
        for image in manifest["images"]:
            image.pop("secure_version")
        self.manifest.write_text(json.dumps(manifest), "utf-8")
        self.assertTrue(all(image["secure_version"] == 0 for image in self.write()["images"]))

    def test_main_writes_the_file_and_says_so(self) -> None:
        stdout = io.StringIO()
        with redirect_stdout(stdout):
            code = packager.main(
                [
                    "index",
                    "--manifest",
                    str(self.manifest),
                    "--channel",
                    "stable",
                    "--out",
                    str(self.root / "hearth-update-stable.json"),
                    "--sequence",
                    "5",
                ]
            )
        self.assertEqual(code, 0)
        self.assertIn(
            "hearth-update-stable.json: channel stable, v0.11.0, sequence 5, 5 images",
            stdout.getvalue(),
        )

    def test_main_reports_an_unreadable_manifest_and_exits_1(self) -> None:
        stderr = io.StringIO()
        with redirect_stderr(stderr):
            code = packager.main(
                [
                    "index",
                    "--manifest",
                    str(self.root / "nothing.json"),
                    "--channel",
                    "beta",
                    "--out",
                    str(self.root / "i.json"),
                ]
            )
        self.assertEqual(code, 1)
        self.assertIn("package_firmware:", stderr.getvalue())


class PackageToAProfile(unittest.TestCase):
    """--profile: the build is held to firmware/hearth-sink/profiles.json before it is published."""

    def setUp(self) -> None:
        self.dir = tempfile.TemporaryDirectory()
        self.root = Path(self.dir.name)
        self.out = self.root / "out"
        self.profiles = self.root / "profiles.json"
        self.profiles.write_text(
            json.dumps(
                {
                    "format": 1,
                    "watch": [],
                    "free": {"ICLFORGE_EXAMPLE_WIFI_*": "the board's own"},
                    "overlays": {},
                    "profiles": {
                        "s": {
                            "target": "esp32s3",
                            "fragments": [],
                            "image": {
                                "name": "hearth-sink-esp32s3",
                                "flash_size": "16MB",
                                "psram": True,
                                "lowest": 0,
                                "highest": 99,
                                "refuse_above": None,
                            },
                            "budget": {},
                            "locked": {"CONFIG_IDF_TARGET": "esp32s3", "CONFIG_SPIRAM": "y"},
                        }
                    },
                }
            ),
            "utf-8",
        )

    def tearDown(self) -> None:
        self.dir.cleanup()

    def build(self, **kwargs) -> Path:
        build = fixture.make_build(self.root / "b", **kwargs)
        digest = sink_profile.profile_hash(sink_profile.load(self.profiles), "s")
        (build / "CMakeCache.txt").write_text(
            "SDKCONFIG_DEFAULTS:UNINITIALIZED=sdkconfig.defaults\n"
            "ICLFORGE_PROFILE:STRING=s\n"
            f"ICLFORGE_PROFILE_HASH:STRING={digest}\n",
            "utf-8",
        )
        return build

    def test_a_build_of_the_profile_is_packaged_and_says_which(self) -> None:
        fragment = packager.package(
            self.build(), "hearth-sink-esp32s3", self.out, "s", self.profiles
        )
        self.assertEqual(fragment["profile"], "s")

    def test_a_build_that_is_not_the_profile_is_refused_with_the_key(self) -> None:
        with self.assertRaisesRegex(packager.PackageError, r"not profile s.*CONFIG_SPIRAM"):
            packager.package(
                self.build(psram=False), "hearth-sink-esp32s3", self.out, "s", self.profiles
            )
        self.assertFalse(self.out.exists())

    def test_an_image_name_the_profile_does_not_publish_is_refused(self) -> None:
        with self.assertRaisesRegex(packager.PackageError, "does not publish an image named"):
            packager.package(self.build(), "hearth-sink-esp32c6", self.out, "s", self.profiles)

    def test_an_unknown_profile_is_refused(self) -> None:
        with self.assertRaisesRegex(packager.PackageError, "no profile 'z'"):
            packager.package(self.build(), "hearth-sink-esp32s3", self.out, "z", self.profiles)

    def test_without_a_profile_a_build_is_packaged_as_it_was(self) -> None:
        fragment = packager.package(self.build(psram=False), "hearth-sink-esp32s3", self.out)
        self.assertEqual(fragment["profile"], "")


if __name__ == "__main__":
    unittest.main()
