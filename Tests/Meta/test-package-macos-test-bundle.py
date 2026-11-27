#!/usr/bin/env python3

# Copyright (c) 2026-present, the Ladyflock developers.
# SPDX-License-Identifier: BSD-2-Clause

import plistlib
import shutil
import subprocess
import sys
import tempfile
import unittest

from pathlib import Path

BUNDLER = Path(__file__).resolve().parents[2] / "Meta" / "package-macos-test-bundle.py"
APP = Path("Ladyflock.app")
EXECUTABLE = Path("Contents/MacOS/Ladyflock")
LIBRARY = "libfixture runtime.dylib"
RELOCATED_INSTALL_NAME = f"@rpath/{LIBRARY}"


class TestPackageMacOSTestBundle(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        toolchain = self.root / "Fixture Toolchain"
        toolchain.mkdir()
        self.external = toolchain / LIBRARY
        self.compile(
            "int fixture(void) { return 42; }",
            "-dynamiclib",
            f"-Wl,-install_name,{self.external}",
            "-o",
            self.external,
        )

        self.build = self.root / "build"
        self.search_directory = self.build / "lib"
        self.search_directory.mkdir(parents=True)
        plugin = self.build / "Qt6/plugins/platforms/libqcocoa.dylib"
        plugin.parent.mkdir(parents=True)
        # Supply the Mach-O plugin file the bundler expects without depending on a Qt build.
        self.compile("void plugin(void) {}", "-dynamiclib", "-o", plugin)

        self.built_app = self.build / "bin" / APP
        executable = self.built_app / EXECUTABLE
        executable.parent.mkdir(parents=True)
        (self.built_app / "Contents/Info.plist").write_bytes(
            plistlib.dumps(
                {
                    "CFBundleExecutable": "Ladyflock",
                    "CFBundleIdentifier": "io.github.kayyo321.Ladyflock.PackagingTest",
                    "CFBundlePackageType": "APPL",
                    "CFBundleVersion": "1",
                }
            )
        )
        self.compile(
            """
            #include <stdio.h>
            int fixture(void);
            int main(void) {
                printf("fixture=%d\\n", fixture());
                return 0;
            }
            """,
            "-x",
            "none",
            self.external,
            "-Wl,-headerpad_max_install_names",
            f"-Wl,-rpath,{self.search_directory}",
            "-o",
            executable,
        )

    def compile(self, source, *arguments):
        result = subprocess.run(
            ["xcrun", "clang", "-x", "c", "-", *arguments], input=source, capture_output=True, text=True
        )
        self.assertEqual(result.returncode, 0, result.stderr)

    def install(self, name):
        stage = self.root / name
        shutil.copytree(self.built_app, stage / "bundle" / APP)
        return stage

    def package(self, stage, archive, *arguments):
        return subprocess.run(
            [
                sys.executable,
                BUNDLER,
                "--build-dir",
                self.build,
                "--install-dir",
                stage,
                "--output",
                archive,
                *arguments,
            ],
            capture_output=True,
            text=True,
        )

    def test_spaced_external_install_name_requires_explicit_inclusion(self):
        archive = self.root / "Ladyflock.tar.gz"
        rejected = self.package(self.install("without-inclusion"), archive)
        self.assertNotEqual(rejected.returncode, 0)
        self.assertIn(f"links {self.external}, which names nothing inside the bundle", rejected.stderr)
        self.assertFalse(archive.exists())

        included = self.package(self.install("with-inclusion"), archive, "--include-dylib", self.external)
        self.assertEqual(included.returncode, 0, included.stderr)
        self.external.unlink()

        unpacked = self.root / "unpacked"
        unpacked.mkdir()
        subprocess.run(["tar", "-xzf", archive, "-C", unpacked], check=True)
        app = unpacked / APP
        self.assertTrue((app / "Contents/lib" / LIBRARY).is_file())
        subprocess.run(["codesign", "--verify", "--deep", "--strict", app], check=True)
        executable = app / EXECUTABLE
        dependencies = subprocess.check_output(["otool", "-L", executable], text=True)
        self.assertIn(RELOCATED_INSTALL_NAME, dependencies)
        self.assertNotIn(str(self.external), dependencies)
        self.assertEqual(subprocess.check_output([executable], text=True), "fixture=42\n")

    def test_rpath_dependency_cannot_escape_through_a_symlink(self):
        stage = self.install("symlink-escape")
        app = stage / "bundle" / APP
        library_directory = app / "Contents/lib"
        library_directory.mkdir()
        (library_directory / LIBRARY).symlink_to(self.external)
        (self.search_directory / LIBRARY).symlink_to(self.external)
        subprocess.run(
            ["install_name_tool", "-change", self.external, RELOCATED_INSTALL_NAME, app / EXECUTABLE], check=True
        )

        archive = self.root / "Ladyflock.tar.gz"
        rejected = self.package(stage, archive)
        self.assertNotEqual(rejected.returncode, 0)
        self.assertIn(f"{RELOCATED_INSTALL_NAME} would copy a library outside", rejected.stderr)
        self.assertFalse(archive.exists())


if __name__ == "__main__":
    unittest.main()
