#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladyflock developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import os
import subprocess
import sys
import tempfile
import unittest

from pathlib import Path

SOURCE_DIR = Path(__file__).resolve().parents[2].as_posix()
CMAKE = os.environ.get("LADYFLOCK_TEST_CMAKE", "cmake")


class TestReleaseConfiguration(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.build = self.root / "build"
        (self.root / "CMakeLists.txt").write_text(
            f"""\
cmake_minimum_required(VERSION 3.25)
project(ReleaseConfiguration VERSION 0.1.0 LANGUAGES NONE)
set(CMAKE_INSTALL_LIBDIR lib)
include("{SOURCE_DIR}/Meta/CMake/cmake_options.cmake")
file(WRITE "${{CMAKE_CURRENT_BINARY_DIR}}/application-id" "${{LADYFLOCK_APPLICATION_ID}}")
configure_file("{SOURCE_DIR}/Libraries/LibWebView/UpdateConfiguration.h.in" UpdateConfiguration.h @ONLY ESCAPE_QUOTES)
add_custom_target(ladybird)
include("{SOURCE_DIR}/UI/Qt/WindowsVersionResource.cmake")
""",
            encoding="utf-8",
        )

    def configure(self, **options):
        return subprocess.run(
            [
                CMAKE,
                "-S",
                str(self.root),
                "-B",
                str(self.build),
                f"-DPython3_EXECUTABLE={sys.executable}",
                *(f"-D{key}={value}" for key, value in options.items()),
            ],
            capture_output=True,
            text=True,
        )

    def test_enabled_release_requires_a_feed(self):
        result = self.configure(LADYFLOCK_VERSION="2026.48.7", LADYBIRD_ENABLE_UPDATE_CHECK="ON")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("requires a Ladyflock-owned LADYFLOCK_UPDATE_FEED_URL", " ".join(result.stderr.split()))

    def test_enabled_release_rejects_malformed_feeds(self):
        for feed in (
            "not-a-url",
            "ftp://updates.example.invalid/releases.json",
            "https:///releases.json",
            "https://updates.example.invalid:70000/releases.json",
            "https://updates..example.invalid/releases.json",
            "https://updates.0x1/releases.json",
            "https://xn--a.example/releases.json",
            "https://[::::]/releases.json",
            "https://192.0.2.256/releases.json",
            "https://updates.example.invalid/the feed/releases.json",
            "https://updates.example.invalid/%gg/releases.json",
        ):
            with self.subTest(feed=feed):
                result = self.configure(
                    LADYFLOCK_VERSION="2026.48.7", LADYBIRD_ENABLE_UPDATE_CHECK="ON", LADYFLOCK_UPDATE_FEED_URL=feed
                )
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("Invalid LADYFLOCK_UPDATE_FEED_URL", result.stderr)

    def test_enabled_release_rejects_the_upstream_feed(self):
        result = self.configure(
            LADYFLOCK_VERSION="2026.48.7",
            LADYBIRD_ENABLE_UPDATE_CHECK="ON",
            LADYFLOCK_UPDATE_FEED_URL="HTTPS://user@Updates.Ladybird.org./releases.json",
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("must not point to ladybird.org", " ".join(result.stderr.split()))

    def test_enabled_release_accepts_valid_feeds(self):
        for feed in (
            "https://updates.example.invalid/ladyflock/releases.json?channel=beta;platform=windows#notes",
            "https://xn--fa-hia.example.invalid/releases.json",
            "http://127.0.0.1:8765/releases.json",
            "http://0x7f.1:8765/releases.json",
            "http://[::ffff:192.0.2.1]:8765/releases.json",
        ):
            with self.subTest(feed=feed):
                result = self.configure(
                    LADYFLOCK_VERSION="2026.48.7", LADYBIRD_ENABLE_UPDATE_CHECK="ON", LADYFLOCK_UPDATE_FEED_URL=feed
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                config_header = (self.build / "UpdateConfiguration.h").read_text(encoding="utf-8")
                self.assertIn(f'#define LADYFLOCK_UPDATE_FEED_URL "{feed}"', config_header)

    def test_development_and_update_disabled_builds_do_not_require_a_feed(self):
        for version, enabled in (("", "ON"), ("2026.48.7", "OFF")):
            with self.subTest(version=version, enabled=enabled):
                result = self.configure(LADYFLOCK_VERSION=version, LADYBIRD_ENABLE_UPDATE_CHECK=enabled)
                self.assertEqual(result.returncode, 0, result.stderr)

    def test_application_id_ignores_legacy_cache_value(self):
        result = self.configure(LADYFLOCK_APPLICATION_ID="org.example.OtherBrowser")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.build / "application-id").read_text(encoding="utf-8"), "io.github.kayyo321.Ladyflock")
        self.assertNotIn("LADYFLOCK_APPLICATION_ID:", (self.build / "CMakeCache.txt").read_text(encoding="utf-8"))

    def test_windows_version_uses_the_configured_product_version(self):
        for version, numeric in (("", "0,1,0,0"), ("2026.048.7", "2026,48,7,0"), ("8.9.10.11", "8,9,10,11")):
            with self.subTest(version=version):
                result = self.configure(LADYFLOCK_VERSION=version, LADYBIRD_COMPATIBILITY_VERSION="2.3.4")
                self.assertEqual(result.returncode, 0, result.stderr)
                resource = (self.build / "ladyflock.rc").read_text(encoding="utf-8")
                self.assertIn(f"FILEVERSION {numeric}\n", resource)
                self.assertIn(f"PRODUCTVERSION {numeric}\n", resource)
                self.assertIn(f'VALUE "FileVersion", "{version or "0.1.0"}"', resource)
                self.assertIn(f'VALUE "ProductVersion", "{version or "0.1.0"}"', resource)

    def test_windows_version_rejects_unrepresentable_components(self):
        for version in ("1.2.3.4.5", "1.65536.0", "1.0-alpha"):
            with self.subTest(version=version):
                result = self.configure(LADYFLOCK_VERSION=version)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("The Windows version resource requires", result.stderr)


if __name__ == "__main__":
    unittest.main()
