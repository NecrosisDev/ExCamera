"""Portable release contracts. Git and compiler calls are mocked, never published."""
from contextlib import redirect_stdout
import importlib.util
import io
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import warnings
import zipfile

SPEC = importlib.util.spec_from_file_location(
    "excamera_release", Path(__file__).resolve().with_name("release.py")
)
RELEASE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RELEASE)
COMMIT = "a" * 40


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        (self.root / "src").mkdir()
        (self.root / "build").mkdir()
        self.header = self.root / "src/build_identity.h"
        self.header.write_text(
            '#define FP_BUILD "0.2.0"\n#define FP_REVISION 47\n', encoding="utf-8"
        )
        root = patch.object(RELEASE, "ROOT", self.root)
        root.start()
        self.addCleanup(root.stop)
        self.output = self.root / "github-output"
        environment = patch.dict(os.environ, {
            "GITHUB_REF_NAME": "v0.3.0-rc.1",
            "GITHUB_REF": "refs/tags/v0.3.0-rc.1",
            "GITHUB_REPOSITORY": "example/ExCamera",
            "GITHUB_OUTPUT": str(self.output),
        })
        environment.start()
        self.addCleanup(environment.stop)
        stdout = redirect_stdout(io.StringIO())
        stdout.__enter__()
        self.addCleanup(stdout.__exit__, None, None, None)

    def package(self, version="0.3.0-rc.1", entries=None):
        entries = entries if entries is not None else {
            name: ("nonempty " + name).encode() for name in RELEASE.EXPECTED_FILES
        }
        path = self.root / "build" / f"ExCamera-{version}-minimal.zip"
        with zipfile.ZipFile(path, "w", zipfile.ZIP_STORED) as archive:
            for name, value in entries.items():
                archive.writestr(name, value)
        return path

    def test_semver_stable_and_prerelease_tags(self):
        for tag in ("v0.0.0", "v0.2.1", "v1.2.3", "v10.20.30-rc.1",
                    "v1.2.3-0", "v1.2.3-alpha-beta.0", "v1.2.3-01a"):
            with self.subTest(tag=tag):
                self.assertEqual(RELEASE.version_from_tag(tag), tag[1:])

    def test_unsafe_or_invalid_tags_rejected(self):
        for tag in ("0.2.0", "V1.2.3", "v01.2.3", "v1.02.3", "v1.2.03",
                    "v1.2", "v1.2.3-", "v1.2.3-01", "v1.2.3-rc.01",
                    "v1.2.3-rc..1", "v1.2.3+build", "v1.2.3/escape",
                    "v1.2.3\nasset=bad", "v1.2.3;echo", "v1.2.3_foo"):
            with self.subTest(tag=tag), self.assertRaises(ValueError):
                RELEASE.version_from_tag(tag)

    def test_context_requires_tag_ref_and_matching_peeled_commit(self):
        with patch.object(RELEASE, "git", return_value=COMMIT) as git:
            self.assertEqual(RELEASE.release_context(), ("v0.3.0-rc.1", "0.3.0-rc.1"))
        self.assertEqual(git.call_args_list[0].args, ("rev-parse", "HEAD"))
        self.assertEqual(git.call_args_list[1].args, (
            "rev-parse", "--verify", "refs/tags/v0.3.0-rc.1^{commit}"
        ))
        with patch.dict(os.environ, {"GITHUB_REF": "refs/heads/v0.3.0-rc.1"}):
            with self.assertRaisesRegex(ValueError, "tag push"):
                RELEASE.release_context()
        with patch.object(RELEASE, "git", side_effect=[COMMIT, "b" * 40]):
            with self.assertRaisesRegex(ValueError, "does not match"):
                RELEASE.release_context()

    def test_prepare_derives_version_preserves_revision_and_sets_all_outputs(self):
        with patch.object(RELEASE, "git", return_value=COMMIT):
            RELEASE.prepare()
        self.assertEqual(self.header.read_text(encoding="utf-8"),
                         '#define FP_BUILD "0.3.0-rc.1"\n#define FP_REVISION 47\n')
        self.assertEqual(self.output.read_text(encoding="utf-8"),
                         "version=0.3.0-rc.1\ntag=v0.3.0-rc.1\n"
                         f"commit={COMMIT}\nasset=build/ExCamera-0.3.0-rc.1-minimal.zip\n")

    def test_prepare_rejects_missing_or_duplicate_build_definition(self):
        for source in ("#define FP_REVISION 47\n",
                       '#define FP_BUILD "0.2.0"\n#define FP_BUILD "0.2.1"\n'):
            with self.subTest(source=source):
                self.header.write_text(source, encoding="utf-8")
                with patch.object(RELEASE, "git", return_value=COMMIT):
                    with self.assertRaisesRegex(ValueError, "exactly one"):
                        RELEASE.prepare()
                self.assertEqual(self.header.read_text(encoding="utf-8"), source)
                self.assertFalse(self.output.exists())

    def test_prepare_tag_movement_changes_neither_header_nor_outputs(self):
        before = self.header.read_bytes()
        with patch.object(RELEASE, "git", side_effect=[COMMIT, "b" * 40]):
            with self.assertRaisesRegex(ValueError, "does not match"):
                RELEASE.prepare()
        self.assertEqual(self.header.read_bytes(), before)
        self.assertFalse(self.output.exists())

    def test_verify_exact_nonempty_four_file_package(self):
        self.package()
        RELEASE.verify_package("0.3.0-rc.1")

    def test_verify_rejects_missing_or_multiple_packages(self):
        with self.assertRaisesRegex(ValueError, "Expected only"):
            RELEASE.verify_package("0.3.0-rc.1")
        self.package()
        self.package("0.2.0")
        with self.assertRaisesRegex(ValueError, "Expected only"):
            RELEASE.verify_package("0.3.0-rc.1")

    def test_verify_rejects_wrong_filename_and_unsafe_version(self):
        self.package("0.2.0")
        with self.assertRaisesRegex(ValueError, "Expected only"):
            RELEASE.verify_package("0.3.0-rc.1")
        for version in ("../evil", "v1/2/3", "0.3.0\n", ""):
            with self.subTest(version=version), self.assertRaisesRegex(ValueError, "Unsafe"):
                RELEASE.verify_package(version)

    def test_verify_rejects_missing_extra_nested_and_empty_entries(self):
        valid = {name: b"bytes" for name in RELEASE.EXPECTED_FILES}
        cases = [
            {key: value for key, value in valid.items() if key != "ExCamera.exe"},
            dict(valid, **{"first-person.ini": b"private settings"}),
            {("folder/" + key if key == "ExCamera.exe" else key): value
             for key, value in valid.items()},
            dict(valid, **{"ExCamera.exe": b""}),
        ]
        for entries in cases:
            with self.subTest(entries=list(entries)):
                self.package(entries=entries)
                with self.assertRaises(ValueError):
                    RELEASE.verify_package("0.3.0-rc.1")

    def test_verify_rejects_duplicate_entries_and_corrupt_payload(self):
        path = self.package()
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", UserWarning)
            with zipfile.ZipFile(path, "a") as archive:
                archive.writestr("ExCamera.exe", b"duplicate")
        with self.assertRaisesRegex(ValueError, "missing or unexpected"):
            RELEASE.verify_package("0.3.0-rc.1")
        self.package()
        with zipfile.ZipFile(path) as archive:
            info = archive.getinfo("ExCamera.exe")
            offset = info.header_offset + 30 + len(info.filename.encode()) + len(info.extra)
        damaged = bytearray(path.read_bytes())
        damaged[offset] ^= 1
        path.write_bytes(damaged)
        with self.assertRaisesRegex(ValueError, "integrity"):
            RELEASE.verify_package("0.3.0-rc.1")

    def test_verify_cli_rejects_header_tag_mismatch_before_package(self):
        with patch.object(RELEASE, "git", return_value=COMMIT):
            with patch.object(sys, "argv", ["release.py", "verify"]):
                with self.assertRaisesRegex(ValueError, "FP_BUILD does not match"):
                    RELEASE.main()

    def test_local_verify_does_not_require_release_environment_or_git(self):
        self.package("0.2.0")
        with patch.object(RELEASE, "git", side_effect=AssertionError("must not call Git")):
            with patch.object(sys, "argv", ["release.py", "verify", "--local"]):
                RELEASE.main()

    def test_notes_first_release_lists_initial_history_and_escapes_subjects(self):
        calls = []
        def git(*args):
            calls.append(args)
            if args[0] == "rev-parse":
                return COMMIT
            if args[0] == "tag":
                return "v0.3.0-rc.1\nnot-a-version\nv01.2.3"
            if args[0] == "log":
                return "b" * 40 + "\tFix [link](https://invalid) @someone <tag> & *format*"
            self.fail("Unexpected Git command: " + repr(args))
        target = self.root / "build/notes.md"
        with patch.object(RELEASE, "git", side_effect=git):
            RELEASE.notes(target)
        text = target.read_text(encoding="utf-8")
        self.assertIn("from the initial history", text)
        self.assertIn("https://github.com/example/ExCamera/commit/" + "b" * 40, text)
        self.assertIn("https://github.com/example/ExCamera/commits/" + COMMIT, text)
        self.assertIn(r"\[link\]\(https://invalid\)", text)
        self.assertIn("&#64;someone &lt;tag&gt; &amp;", text)
        self.assertEqual(calls[-1], ("log", "--reverse", "--format=%H%x09%s",
                                    "refs/tags/v0.3.0-rc.1", "--"))

    def test_notes_uses_describe_selected_valid_tag_not_lexical_order(self):
        calls = []
        def git(*args):
            calls.append(args)
            if args[0] == "rev-parse":
                return COMMIT
            if args[0] == "tag":
                return "v0.3.0-rc.1\nv9.0.0\nv0.2.1\ninvalid"
            if args[0] == "describe":
                return "v0.2.1"
            if args[0] == "log":
                return COMMIT + "\tDirect commit"
            self.fail("Unexpected Git command: " + repr(args))
        target = self.root / "build/notes.md"
        with patch.object(RELEASE, "git", side_effect=git):
            RELEASE.notes(target)
        self.assertIn(("describe", "--tags", "--abbrev=0", "--match", "v9.0.0",
                       "--match", "v0.2.1", "HEAD"), calls)
        self.assertEqual(calls[-1], ("log", "--reverse", "--format=%H%x09%s",
                                    "refs/tags/v0.2.1..refs/tags/v0.3.0-rc.1", "--"))
        self.assertIn("compare/tags/v0.2.1...tags/v0.3.0-rc.1",
                      target.read_text(encoding="utf-8"))

    def test_same_named_branch_cannot_redirect_changelog_refs(self):
        def git(*args):
            if args[0] == "rev-parse":
                return COMMIT
            if args[0] == "tag":
                return "v0.3.0-rc.1\nv0.2.1"
            if args[0] == "describe":
                return "v0.2.1"
            if args == ("log", "--reverse", "--format=%H%x09%s",
                        "refs/tags/v0.2.1..refs/tags/v0.3.0-rc.1", "--"):
                return COMMIT + "\tTagged change"
            if args[0] == "log":
                return "b" * 40 + "\tWrong branch change"
            self.fail("Unexpected Git command: " + repr(args))
        target = self.root / "build/notes.md"
        with patch.object(RELEASE, "git", side_effect=git):
            RELEASE.notes(target)
        text = target.read_text(encoding="utf-8")
        self.assertIn("Tagged change", text)
        self.assertNotIn("Wrong branch change", text)
        self.assertIn("/compare/tags/v0.2.1...tags/v0.3.0-rc.1", text)

    def test_workflow_remote_identity_uses_explicit_tag_before_publication(self):
        workflow = (Path(__file__).resolve().parents[1] /
                    ".github/workflows/release.yml").read_text(encoding="utf-8")
        lookup = 'gh api "repos/$env:GITHUB_REPOSITORY/commits/refs/tags/$env:RELEASE_TAG"'
        self.assertIn(lookup, workflow)
        self.assertLess(workflow.index(lookup), workflow.index("gh @arguments"))
        self.assertIn("$remoteCommit -cne $env:RELEASE_COMMIT", workflow)
        self.assertIn("'--verify-tag'", workflow)

    def test_notes_with_no_new_commits_and_invalid_repository(self):
        def git(*args):
            return {"rev-parse": COMMIT, "tag": "v0.3.0-rc.1\nv0.2.1",
                    "describe": "v0.2.1", "log": ""}[args[0]]
        target = self.root / "build/notes.md"
        with patch.object(RELEASE, "git", side_effect=git):
            RELEASE.notes(target)
            self.assertIn("No new commits", target.read_text(encoding="utf-8"))
            with patch.dict(os.environ, {"GITHUB_REPOSITORY": "bad owner/repo"}):
                with self.assertRaisesRegex(ValueError, "owner/repository"):
                    RELEASE.notes(target)


if __name__ == "__main__":
    unittest.main(verbosity=2)
