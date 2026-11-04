"""Standard-library unit tests for build.py, release.py, check_generated.py, and contract tools."""
from contextlib import redirect_stdout
import io
import json
import msvcrt
import os
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock
import zipfile

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / 'tools'
for candidate in (str(ROOT), str(TOOLS)):
    if candidate not in sys.path:
        sys.path.insert(0, candidate)

import build  # noqa: E402
import check_generated  # noqa: E402
import native_contracts  # noqa: E402
import release  # noqa: E402
import run_cpp_tests  # noqa: E402
import signatures  # noqa: E402
import xr_contracts  # noqa: E402


class ReleaseToolTests(unittest.TestCase):
    def test_version_from_tag_accepts_valid_semver(self):
        self.assertEqual(release.version_from_tag('v0.3.0'), '0.3.0')
        self.assertEqual(release.version_from_tag('v0.0.0'), '0.0.0')
        self.assertEqual(release.version_from_tag('v1.2.3-rc.1'), '1.2.3-rc.1')
        self.assertEqual(release.version_from_tag('v10.20.30-alpha.0.beta-2'), '10.20.30-alpha.0.beta-2')
        self.assertEqual(release.version_from_tag('v0.3.0-01a'), '0.3.0-01a')

    def test_version_from_tag_rejects_invalid_tags(self):
        invalid = [
            '',
            'main',
            '0.3.0',
            'V0.3.0',
            'v0.3',
            'v0.3.0.1',
            'v01.0.0',
            'v0.03.0',
            'v0.3.01',
            'v0.3.0-rc.01',
            'v0.3.0-00',
            'v0.3.0+1',
            'v1.2.3-rc.1+build.4',
            'v1.2.3-',
            'v1.2.3-rc..1',
        ]
        for tag in invalid:
            with self.subTest(tag=tag):
                with self.assertRaises(ValueError):
                    release.version_from_tag(tag)

    def test_release_context_supports_tag_push_and_input_tag(self):
        commit_sha = 'a' * 40

        def fake_git(*args):
            if args == ('rev-parse', 'HEAD'):
                return commit_sha
            if args == ('rev-parse', '--verify', 'refs/tags/v0.3.0^{commit}'):
                return commit_sha
            raise AssertionError(f'Unexpected git args: {args}')

        with mock.patch.object(release, 'git', side_effect=fake_git):
            with mock.patch.dict(
                os.environ,
                {'INPUT_TAG': '', 'GITHUB_REF_NAME': 'v0.3.0', 'GITHUB_REF': 'refs/tags/v0.3.0'},
                clear=False,
            ):
                self.assertEqual(release.release_context(), ('v0.3.0', '0.3.0'))

            with mock.patch.dict(
                os.environ,
                {'INPUT_TAG': 'v0.3.0', 'GITHUB_REF_NAME': 'main', 'GITHUB_REF': 'refs/heads/main'},
                clear=False,
            ):
                self.assertEqual(release.release_context(), ('v0.3.0', '0.3.0'))

            with mock.patch.dict(
                os.environ,
                {'INPUT_TAG': '', 'GITHUB_REF_NAME': 'main', 'GITHUB_REF': 'refs/heads/main'},
                clear=False,
            ):
                with self.assertRaises(ValueError):
                    release.release_context()

        with mock.patch.object(
            release,
            'git',
            side_effect=lambda *args: 'a' * 40 if args == ('rev-parse', 'HEAD') else 'b' * 40,
        ):
            with mock.patch.dict(os.environ, {'INPUT_TAG': 'v0.3.0'}, clear=False):
                with self.assertRaisesRegex(ValueError, 'does not match the release tag'):
                    release.release_context()

    def _write_minimal_zip(self, zip_path: Path, version: str = '0.3.0', overrides=None, omit=(), extra=None):
        entries = {
            'ExCamera.exe': b'MZ-launcher',
            'ExanimaFirstPerson.dll': b'MZ-runtime',
            'manifest.toml': f'[plugin]\nname = "ExCamera"\nversion = "{version}"\n'.encode('utf-8'),
            'README.txt': b'Player readme\n',
            'LICENSE.txt': b'MIT License\n',
            'LICENSE-MinHook.txt': b'MinHook License\n',
        }
        if overrides:
            entries.update(overrides)
        for key in omit:
            entries.pop(key, None)
        if extra:
            entries.update(extra)
        with zipfile.ZipFile(zip_path, 'w', zipfile.ZIP_DEFLATED) as archive:
            for name, payload in entries.items():
                archive.writestr(name, payload)

    def test_verify_package_accepts_valid_minimal_zip(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            self._write_minimal_zip(out / 'ExCamera-0.3.0-minimal.zip', version='0.3.0')
            buf = io.StringIO()
            with redirect_stdout(buf):
                release.verify_package('0.3.0', build_dir=out)
            self.assertIn('Ready:', buf.getvalue())

    def test_verify_package_rejects_missing_extra_empty_and_mismatched_manifest(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            # Missing ZIP archive
            with self.assertRaisesRegex(ValueError, 'Expected only ExCamera-0.3.0-minimal.zip'):
                release.verify_package('0.3.0', build_dir=out)

            zip_path = out / 'ExCamera-0.3.0-minimal.zip'

            # Missing required entry
            self._write_minimal_zip(zip_path, omit=('LICENSE-MinHook.txt',))
            with self.assertRaisesRegex(ValueError, 'missing or unexpected files'):
                release.verify_package('0.3.0', build_dir=out)

            # Unexpected extra entry
            self._write_minimal_zip(zip_path, extra={'first-person.ini': b'[Camera]\nFOV=90\n'})
            with self.assertRaisesRegex(ValueError, 'missing or unexpected files'):
                release.verify_package('0.3.0', build_dir=out)

            # Empty required file
            self._write_minimal_zip(zip_path, overrides={'README.txt': b''})
            with self.assertRaisesRegex(ValueError, 'Empty release file: README.txt'):
                release.verify_package('0.3.0', build_dir=out)

            # Mismatched manifest.toml version
            self._write_minimal_zip(zip_path, version='0.0.0')
            with self.assertRaisesRegex(ValueError, 'manifest.toml version'):
                release.verify_package('0.3.0', build_dir=out)

            # Extra minimal ZIP sitting alongside expected ZIP
            self._write_minimal_zip(zip_path, version='0.3.0')
            self._write_minimal_zip(out / 'ExCamera-0.2.9-minimal.zip', version='0.2.9')
            with self.assertRaisesRegex(ValueError, 'Expected only ExCamera-0.3.0-minimal.zip'):
                release.verify_package('0.3.0', build_dir=out)

        with self.assertRaisesRegex(ValueError, 'Unsafe package version'):
            release.verify_package('../0.3.0')


class GeneratedAndContractTests(unittest.TestCase):
    def test_real_repository_generated_tables_are_consistent(self):
        buf = io.StringIO()
        with redirect_stdout(buf):
            check_generated.check()
        output = buf.getvalue()
        self.assertIn('XR contracts', output)
        self.assertIn('consistent', output)

    def test_relocation_bisect_matches_naive_scan(self):
        relocations = sorted([0x1000, 0x1010, 0x1018, 0x2050])
        for address in range(0x0FF8, 0x2060):
            expected = any(reloc <= address < reloc + 8 for reloc in relocations)
            self.assertEqual(xr_contracts.is_relocated_byte(relocations, address), expected)
        self.assertFalse(xr_contracts.is_relocated_byte([], 0x1000))

    def test_xr_contracts_offline_detects_synthetic_drift(self):
        spec = {
            'source_sha256': 'a' * 64,
            'owners': {'ownerAlpha': '0x1200', 'ownerBeta': '0x1400'},
            'leaf_owners': ['ownerBeta'],
            'frustum_template': {'rva': '0x3000', 'indices': [1, 2, 3, 4]},
        }
        owner_tokens = [
            ('ownerAlpha', 0x1200, ['48', '89', '5C', '24', '??', 'C3']),
            ('ownerBeta', 0x1400, ['40', '53', '48', '83', 'EC', '20', 'C3']),
        ]
        header = xr_contracts.render_header(owner_tokens, spec['frustum_template'])
        stats = xr_contracts.validate_offline(spec, header, expected_source_sha256='a' * 64)
        self.assertEqual(stats['owners'], 2)
        self.assertEqual(stats['bytes'], 13)

        # Build SHA-256 drift
        with self.assertRaisesRegex(ValueError, 'different reviewed source build'):
            xr_contracts.validate_offline(spec, header, expected_source_sha256='b' * 64)

        # RVA drift
        bad_rva = header.replace('{0x1200,', '{0x1204,')
        with self.assertRaisesRegex(ValueError, 'entry mismatch'):
            xr_contracts.validate_offline(spec, bad_rva)

        # Order drift
        swapped = xr_contracts.render_header(list(reversed(owner_tokens)), spec['frustum_template'])
        with self.assertRaisesRegex(ValueError, 'entry mismatch'):
            xr_contracts.validate_offline(spec, swapped)

        # Trivial all-wildcard mask
        all_wild = xr_contracts.render_header(
            [('ownerAlpha', 0x1200, ['??', '??']), owner_tokens[1]],
            spec['frustum_template'],
        )
        with self.assertRaisesRegex(ValueError, 'trivial all-wildcard mask'):
            xr_contracts.validate_offline(spec, all_wild)

        # Invalid hex token
        bad_tok = header.replace('48 89', 'ZZ 89')
        with self.assertRaisesRegex(ValueError, 'XR contract count mismatch|Invalid byte/mask token'):
            xr_contracts.validate_offline(spec, bad_tok)

        # Hand-edited admitted() template check
        tampered_footer = header.replace('templateIndices[4] = {1, 2, 3, 4}', 'templateIndices[4] = {1, 2, 3, 9}')
        with self.assertRaisesRegex(ValueError, 'stale or hand-edited'):
            xr_contracts.validate_offline(spec, tampered_footer)

        # Undeclared leaf owner
        bad_leaf_spec = dict(spec, leaf_owners=['missingLeaf'])
        with self.assertRaisesRegex(ValueError, 'Undeclared XR leaf_owners'):
            xr_contracts.validate_offline(bad_leaf_spec, header)

    def test_signatures_rendering_determinism(self):
        spec = {
            'source_sha256': 'c' * 64,
            'source_image_base': '0x140000000',
            'target_order': ['fnTick', 'globalState', 'vtableSlot'],
            'functions': {'fnTick': {'rva': '0x2000', 'hook': True}},
            'returns': {},
            'globals': {'globalState': {'rva': '0x5000', 'optionalGroup': 'viewEffects'}},
            'slots': {'vtableSlot': {'vtable': '0x6000', 'offset': '0x10', 'holds': 'fnTick'}},
            'reviewed_signatures': [
                {
                    'target': 'fnTick',
                    'kind': 'Code',
                    'hook': True,
                    'entry': True,
                    'offset': 0,
                    'insn_end': 0,
                    'addend': 0,
                    'known_rva': '0x2000',
                    'pattern': '48 89 5C 24 ?? 57 48 83 EC 20',
                },
                {
                    'target': 'globalState',
                    'kind': 'Relative',
                    'hook': False,
                    'entry': False,
                    'offset': 3,
                    'insn_end': 7,
                    'addend': 0,
                    'known_rva': '0x5000',
                    'pattern': '48 8B 05 ?? ?? ?? ?? 48 85 C0',
                },
                {
                    'target': 'vtableSlot',
                    'kind': 'Relative',
                    'hook': False,
                    'entry': False,
                    'offset': 3,
                    'insn_end': 7,
                    'addend': 16,
                    'known_rva': '0x6010',
                    'pattern': '48 8D 05 ?? ?? ?? ?? 48 89 01',
                },
            ],
        }
        entries = signatures.reviewed_entries(spec)
        groups = signatures.optional_groups(spec)
        slots = [(name, item['holds']) for name, item in spec['slots'].items()]
        exe_stub = SimpleNamespace(base=int(spec['source_image_base'], 16))

        rendered_a = signatures.generated_header(entries, slots, exe_stub, groups, spec['target_order'])
        rendered_b = signatures.generated_header(entries, slots, exe_stub, groups, spec['target_order'])
        self.assertEqual(rendered_a, rendered_b)
        self.assertEqual(signatures.parse_optional_groups(rendered_a, spec['target_order']), {'globalState': 'viewEffects'})
        self.assertIn('{vtableSlot, fnTick},', rendered_a)

    def test_native_contracts_rendering_determinism(self):
        structs_src = """
        namespace actor {
            inline constexpr size_t health = 0x40, maxHealth = health + 0x04;
        }
        namespace bindings {
            inline constexpr size_t count = 1;
        }
        """
        offsets = native_contracts.field_offsets(structs_src)
        self.assertEqual(offsets, {'actor::health': 0x40, 'actor::maxHealth': 0x44, 'bindings::count': 1})

        spec = {
            'schema': 1,
            'source_sha256': 'd' * 64,
            'owners': [
                {
                    'name': 'updateActor',
                    'target': 'fnTick',
                    'rva': '0x2000',
                    'end_rva': '0x2040',
                    'entry_offset': 0,
                    'pattern': '48 89 5C 24 ?? 57',
                }
            ],
            'fields': [
                {
                    'name': 'actor::health',
                    'value': '0x40',
                    'owner': 'updateActor',
                    'instruction': '0x2008',
                    'window_offset': 6,
                    'pattern': 'F3 0F 10 41 40',
                    'width': 4,
                    'provenance': 'reviewed test instruction',
                }
            ],
            'derived': [
                {
                    'name': 'actor::maxHealth',
                    'value': '0x44',
                    'base': 'actor::health',
                    'add': 4,
                    'provenance': 'adjacent float field',
                }
            ],
            'references': [
                {
                    'name': 'ref::key0',
                    'owner': 'updateActor',
                    'instruction': '0x2010',
                    'window_offset': 16,
                    'pattern': '8B 05 ?? ?? ?? ??',
                    'relative_to': 'globalKeys',
                    'displacement_offset': 2,
                    'next_offset': 6,
                    'relative_add': 0,
                    'width': 4,
                    'provenance': 'reviewed global reference',
                }
            ],
            'constants': [
                {
                    'name': 'bindings::count',
                    'value': 1,
                    'kind': 'mod_scan_bound',
                    'provenance': 'single binding entry',
                }
            ],
        }
        native_contracts.validate_spec(spec, offsets)
        header_a = native_contracts.generated_header(spec)
        header_b = native_contracts.generated_header(spec)
        self.assertEqual(header_a, header_b)
        self.assertIn('static_assert(game::actor::health == 0x40', header_a)
        self.assertIn('static_assert(game::actor::maxHealth == 0x44', header_a)

    def test_fpc_vmt_and_shifted_slot_discovery(self):
        mem = bytearray(0x3000)
        base = 0x140000000
        mem[0x2100:0x2108] = b'\x07T3DNode'
        mem[0x2200:0x2208] = (0x1C8).to_bytes(8, 'little', signed=True)
        mem[0x2208:0x2210] = (-0x1C8).to_bytes(8, 'little', signed=True)
        mem[0x2210:0x2218] = (0).to_bytes(8, 'little')
        mem[0x2218:0x2220] = (base + 0x2100).to_bytes(8, 'little')
        mem[0x2260:0x2268] = (base + 0x1100).to_bytes(8, 'little')
        mem[0x2268:0x2270] = (base + 0x1200).to_bytes(8, 'little')
        mem[0x2270:0x2278] = (base + 0x1300).to_bytes(8, 'little')
        exe = SimpleNamespace(mem=mem, text=(0x1000, 0x2000), base=base)

        self.assertEqual(signatures.find_fpc_vmt(exe, 'T3DNode'), 0x2200)
        self.assertEqual(signatures.find_vmt_method_slot(exe, 0x2200, 0x1200, 0x68), 0x2268)

        # Base-class virtual insertion shifts 0x1200 from +0x68 -> +0x70
        mem[0x2268:0x2270] = (base + 0x1180).to_bytes(8, 'little')
        mem[0x2270:0x2278] = (base + 0x1200).to_bytes(8, 'little')
        mem[0x2278:0x2280] = (base + 0x1300).to_bytes(8, 'little')
        self.assertEqual(signatures.find_vmt_method_slot(exe, 0x2200, 0x1200, 0x68), 0x2270)

        # Duplicate method pointer in VMT fails closed
        mem[0x2278:0x2280] = (base + 0x1200).to_bytes(8, 'little')
        self.assertIsNone(signatures.find_vmt_method_slot(exe, 0x2200, 0x1200, 0x68))
        mem[0x2278:0x2280] = (base + 0x1300).to_bytes(8, 'little')

        # Duplicate VMT class name fails closed
        mem[0x2500:0x2580] = mem[0x2200:0x2280]
        self.assertIsNone(signatures.find_fpc_vmt(exe, 'T3DNode'))

    def test_native_contracts_scoped_degradation_and_closure_verification(self):
        mem = bytearray(b'\xCC' * 0x3000)
        base = 0x140000000
        # Owner at 0x1100: sub rsp, 0x28; call 0x1300; add rsp, 0x28; ret
        mem[0x1100:0x110E] = bytes([0x48, 0x83, 0xEC, 0x28, 0xE8, 0xF7, 0x01, 0x00, 0x00, 0x48, 0x83, 0xC4, 0x28, 0xC3])
        # Callee at 0x1300
        mem[0x1300:0x1308] = bytes([0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x5B, 0xC3])

        def find_fn(data, mask, limit=3):
            import re as _re
            rx = _re.compile(b''.join(_re.escape(bytes([b])) if m else b'.' for b, m in zip(data, mask)), _re.DOTALL)
            return [0x1000 + m.start() for m in rx.finditer(bytes(mem[0x1000:0x2000]))][:limit]

        exe = SimpleNamespace(mem=mem, text=(0x1000, 0x2000), base=base, find=find_fn)
        layout = {
            'ok': True,
            'rvas': {'fnCore': 0x1100, 'invHook': 0x1400, 'throwHook': 0x1500},
            'groups': {'invHook': 'inventoryReach', 'throwHook': 'heldThrow'},
        }
        spec = {
            'owners': [
                {
                    'name': 'ownerCore',
                    'target': 'fnCore',
                    'rva': '0x1100',
                    'end_rva': '0x110E',
                    'entry_offset': 0,
                    'pattern': '48 83 EC 28 E8 ?? ?? ?? ?? 48 83 C4 28 C3',
                }
            ],
            'fields': [
                {
                    'name': 'core::ok',
                    'owner': 'ownerCore',
                    'window_offset': 0,
                    'pattern': '48 83 EC 28',
                },
                {
                    'name': 'inventory::changed',
                    'owner': 'ownerCore',
                    'window_offset': 0,
                    'pattern': '48 83 EC 38',
                    'optional_group': 'inventoryReach',
                },
                {
                    'name': 'heldThrow::body',
                    'owner': 'ownerCore',
                    'window_offset': 0,
                    'pattern': '48 83 EC 28 E8 ?? ?? ?? ?? 48 83 C4 28 C3',
                    'optional_group': 'heldThrow',
                },
            ],
            'references': [],
            'closures': {
                'callees': [{'name': 'callee_1300', 'pattern': '40 53 48 83 EC 20 5B C3'}],
                'branches': [
                    {
                        'group': 'heldThrow',
                        'witness': 'heldThrow::body',
                        'disp_offset': 5,
                        'insn_end': 9,
                        'kind': 'callee',
                        'callee': 'callee_1300',
                    }
                ],
                'vmt_slots': [],
            },
        }
        res = native_contracts.resolve_contracts(exe, spec, layout)
        self.assertTrue(res['ok'])
        self.assertEqual(res['disabled'], {'invHook'})
        self.assertEqual(res['rvas']['fnCore'], 0x1100)
        self.assertEqual(res['rvas']['invHook'], 0)
        self.assertEqual(res['rvas']['throwHook'], 0x1500)

        # Retargeting the E8 rel32 call fails the heldThrow closure without failing core
        mem[0x1105] = 0x07
        res_tampered = native_contracts.resolve_contracts(exe, spec, layout)
        self.assertTrue(res_tampered['ok'])
        self.assertIn('heldThrow', res_tampered['closure_failed_groups'])
        self.assertEqual(res_tampered['rvas']['throwHook'], 0)
        self.assertEqual(res_tampered['rvas']['fnCore'], 0x1100)


class BuildHelperTests(unittest.TestCase):
    def test_read_define_parses_single_definition(self):
        self.assertEqual(build.read_define('#pragma once\n#define FP_BUILD "0.3.0"\n', 'FP_BUILD'), '0.3.0')
        with self.assertRaises(ValueError):
            build.read_define('#pragma once\n', 'FP_BUILD')
        with self.assertRaises(ValueError):
            build.read_define('#define FP_BUILD "0.3.0"\n#define FP_BUILD "0.3.1"\n', 'FP_BUILD')

    def test_validate_manifest_checks_structure_and_version(self):
        valid = '[plugin]\nname = "ExCamera"\nversion = "0.3.0"\n'
        self.assertEqual(build.validate_manifest(valid), '0.3.0')
        self.assertEqual(build.validate_manifest(valid, expected_version='0.3.0'), '0.3.0')
        with self.assertRaisesRegex(ValueError, 'does not match'):
            build.validate_manifest(valid, expected_version='0.3.1')
        with self.assertRaisesRegex(ValueError, 'missing \\[plugin\\]'):
            build.validate_manifest('name = "ExCamera"\nversion = "0.3.0"\n')
        with self.assertRaisesRegex(ValueError, 'must declare name = "ExCamera"'):
            build.validate_manifest('[plugin]\nname = "OtherMod"\nversion = "0.3.0"\n')

    def test_resolve_openxr_sdk_loader_and_license_discovery(self):
        with tempfile.TemporaryDirectory(prefix='excamera_openxr_ü_') as tmp:
            sdk = Path(tmp)
            with self.assertRaisesRegex(FileNotFoundError, 'OpenXR headers missing'):
                build.resolve_openxr_sdk(sdk)

            header = sdk / 'include/openxr/openxr.h'
            header.parent.mkdir(parents=True)
            header.write_text('/* stub */\n', encoding='utf-8')

            info_no_loader = build.resolve_openxr_sdk(sdk, require_loader=False)
            self.assertIsNone(info_no_loader['loader'])

            with self.assertRaisesRegex(FileNotFoundError, 'xr_stereo\\.inl requires openxr_loader\\.dll'):
                build.resolve_openxr_sdk(sdk, require_loader=True)

            loader = sdk / 'x64/bin/openxr_loader.dll'
            loader.parent.mkdir(parents=True)
            loader.write_bytes(b'MZ-openxr-loader')
            lic = sdk / 'LICENSE'
            lic.write_text('Apache-2.0\n', encoding='utf-8')

            info = build.resolve_openxr_sdk(sdk, require_loader=True)
            self.assertEqual(info['loader'], loader.resolve())
            self.assertEqual(info['license'], lic.resolve())

    def test_build_lock_raises_clear_runtime_error_when_held(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            lock_file = out / '.build.lock'
            with lock_file.open('a+b') as held:
                held.seek(0)
                msvcrt.locking(held.fileno(), msvcrt.LK_NBLCK, 1)
                try:
                    with self.assertRaisesRegex(
                        RuntimeError,
                        r'Another build\.py process is already running \(build/\.build\.lock is held\)',
                    ):
                        with build.build_lock(out):
                            pass
                finally:
                    held.seek(0)
                    msvcrt.locking(held.fileno(), msvcrt.LK_UNLCK, 1)

            entered = False
            with build.build_lock(out):
                entered = True
            self.assertTrue(entered)

    def test_cpp_test_runner_links_settings_presentation_when_needed(self):
        self.assertEqual(
            run_cpp_tests.extra_sources_for(ROOT / 'tests/unit/settings_model_tests.cpp'),
            [build.SRC / 'settings_presentation.cpp'],
        )
        self.assertEqual(
            run_cpp_tests.extra_sources_for(ROOT / 'tests/unit/core_headers_tests.cpp'),
            [],
        )


if __name__ == '__main__':
    unittest.main()
