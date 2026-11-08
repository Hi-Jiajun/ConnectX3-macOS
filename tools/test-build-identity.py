#!/usr/bin/env python3
"""Catch stale kmod version generation and mismatched staged build tags.

Runs the real staging helper on fixture plists and compiles a consumer of its
generated header. Rejects malformed metadata before touching output files.
Never loads a kext, opens IOKit, or rewrites the source template.
"""
import importlib.util
import plistlib
import re
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).with_name("stage-build-identity.py")
spec = importlib.util.spec_from_file_location("stage_build_identity", SCRIPT)
stager = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stager)


class BuildIdentityTests(unittest.TestCase):
    def test_default_build_tag_explicitly_marks_unversioned_build(self):
        build = SCRIPT.parents[1] / "build.sh"
        assignment = re.search(r'^BUILD_TAG=.*$', build.read_text(), re.MULTILINE).group(0)
        ran = subprocess.run(["/bin/bash", "-c", 'unset BUILD_TAG\n' + assignment + '\nprintf "%s" "$BUILD_TAG"'],
                             capture_output=True, text=True, check=True)
        self.assertEqual(ran.stdout, "unversioned-local")

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="cx3-build-identity-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / "source.plist"
        self.plist = self.root / "staged.plist"
        self.header = self.root / "build-identity.h"
        self.info = {"CFBundleVersion": "0.44.48", "CFBundleShortVersionString": "0.44.48",
                     "CFBundleIdentifier": "com.hijiajun.driver.Cx3Mlx4",
                     "IOKitPersonalities": {"fixture": {"IOPCIPrimaryMatch": "0x100715b3"}}}
        self.source.write_bytes(plistlib.dumps(self.info))

    def stage(self, tag="0.44.119-local"):
        return stager.stage_identity(self.source, self.plist, self.header, tag)

    def test_release_version_and_candidate_tag_have_separate_matching_metadata(self):
        original = self.source.read_bytes()
        self.stage()
        self.assertTrue(self.plist.exists(), "staging must produce the bundle metadata")
        self.assertTrue(self.header.exists(), "staging must produce compiler identity input")
        saved = plistlib.loads(self.plist.read_bytes())
        self.assertEqual(saved["CFBundleVersion"], "0.44.48")
        self.assertEqual(saved["CFBundleShortVersionString"], "0.44.48")
        self.assertEqual(saved["CX3BuildTag"], "0.44.119-local")
        self.assertEqual(saved["IOKitPersonalities"], self.info["IOKitPersonalities"])
        self.assertEqual(self.source.read_bytes(), original)

    def test_compiled_consumer_gets_template_version_not_stale_literal(self):
        self.info["CFBundleVersion"] = "1.2.3"
        self.source.write_bytes(plistlib.dumps(self.info))
        self.stage("candidate-1")
        self.assertTrue(self.header.exists(), "compiler input must be generated")
        binary = self.root / "consumer"
        built = subprocess.run(["xcrun", "clang", "-Wall", "-Wextra", "-Werror",
                                "-include", str(self.header), "-x", "c", "-",
                                "-o", str(binary)], input=(
            '#include <stdio.h>\nint main(void) { puts(CX3_BUNDLE_VERSION); '
            'puts(CX3_BUILD_TAG); return 0; }\n'), text=True, capture_output=True)
        self.assertEqual(built.returncode, 0, built.stderr)
        ran = subprocess.run([str(binary)], text=True, capture_output=True)
        self.assertEqual(ran.returncode, 0, ran.stderr)
        self.assertEqual(ran.stdout, "1.2.3\ncandidate-1\n")

    def test_bad_tag_is_rejected_before_any_output_write(self):
        self.plist.write_bytes(b"keep-plist")
        self.header.write_bytes(b"keep-header")
        for tag in ("", "with space", 'quote"', "line\nbreak", "汉字", "a"*65):
            with self.subTest(tag=tag), self.assertRaises(ValueError):
                self.stage(tag)
        self.assertEqual(self.plist.read_bytes(), b"keep-plist")
        self.assertEqual(self.header.read_bytes(), b"keep-header")

    def test_missing_or_unsafe_bundle_version_does_not_fall_back(self):
        for version in (None, "", "stale", '0.1"', "1.2.3.4", 123):
            self.info["CFBundleVersion"] = version
            if version is None:
                del self.info["CFBundleVersion"]
            self.source.write_bytes(plistlib.dumps(self.info))
            with self.subTest(version=version), self.assertRaises(ValueError):
                self.stage()
            self.assertFalse(self.plist.exists())
            self.assertFalse(self.header.exists())

    def test_source_and_output_collisions_cannot_rewrite_template(self):
        original = self.source.read_bytes()
        for output_plist, output_header in ((self.source, self.header),
                                           (self.plist, self.source),
                                           (self.plist, self.plist)):
            with self.subTest(plist=output_plist, header=output_header), self.assertRaises(ValueError):
                stager.stage_identity(self.source, output_plist, output_header, "candidate-1")
        self.assertEqual(self.source.read_bytes(), original)

    def test_cli_returns_failure_without_success_output_for_invalid_tag(self):
        result = subprocess.run([sys.executable, str(SCRIPT), str(self.source),
                                 str(self.plist), str(self.header), 'bad"tag'],
                                capture_output=True, text=True, timeout=5)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, "")
        self.assertFalse(self.plist.exists())
        self.assertFalse(self.header.exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
