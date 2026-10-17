"""Native-layout admission on owned image fixtures; optional PE packages are unnecessary."""
import copy
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import Mock, patch

import native_contracts as contracts
from test_signatures import Image


class NativeContractTests(unittest.TestCase):
    def setUp(self):
        self.image = Image()
        self.image.mem[0x130:0x137] = bytes.fromhex('48 8B 83 40 0D 00 00')
        self.image.mem[0x180:0x182] = bytes.fromhex('EE 11')
        self.image.mem[0x1B0:0x1B7] = bytes.fromhex('80 BB F3 12 00 00 00')
        self.image.mem[0x150:0x153] = bytes.fromhex('0F B7 0D')
        self.image.mem[0x153:0x157] = (0x300 + 0x90 - 0x157).to_bytes(4, 'little', signed=True)
        self.spec = {
            'owners': [
                {'name': 'core', 'target': 'playerBehavior', 'pattern': 'AA BB', 'entry_offset': 0},
                {'name': 'guard', 'target': 'defenseCleanup', 'pattern': 'EE 11', 'entry_offset': 0},
            ],
            'fields': [
                {'name': 'character::behaviour', 'owner': 'core', 'window_offset': 0x30,
                 'pattern': '48 8B 83 40 0D 00 00'},
                {'name': 'character::defenseActive', 'owner': 'guard', 'window_offset': 0x30,
                 'pattern': '80 BB F3 12 00 00 00', 'optional_group': 'manualDefense'},
            ],
            'references': [
                {'name': 'binding::interact', 'owner': 'core', 'window_offset': 0x50,
                 'pattern': '0F B7 0D ?? ?? ?? ??', 'relative_to': 'movementKeys', 'relative_add': 0x90,
                 'displacement_offset': 3, 'next_offset': 7},
            ],
        }
        self.layout = {'ok': True, 'rvas': {'playerBehavior': 0x100, 'movementKeys': 0x300,
                                          'defenseCleanup': 0x180, 'defenseReactive': 0x190,
                                          'defenseAttackUpdate': 0x1A0, 'defenseMotionSelect': 0x1B0},
                       'groups': {name: 'manualDefense' for name in (
                           'defenseCleanup', 'defenseReactive', 'defenseAttackUpdate', 'defenseMotionSelect')}}

    def result(self):
        return contracts.resolve_contracts(self.image, self.spec, self.layout)

    def test_valid_layout_and_reference(self):
        result = self.result()
        self.assertTrue(result['ok'])
        self.assertEqual(result['checked'], 3)
        self.assertEqual(result['rvas'], self.layout['rvas'])
        self.assertFalse(result['why'] or result['disabled'])

    def test_field_width_and_offset_change_with_intact_owner(self):
        for byte in (0x130, 0x133):
            with self.subTest(byte=byte):
                self.image.mem[byte] ^= 8 if byte == 0x130 else 1
                self.assertEqual(self.image.find(b'\xaa\xbb', [1, 1]), [0x100])
                result = self.result()
                self.assertFalse(result['ok'])
                self.assertIsNone(result['rvas'])
                self.assertEqual(result['why']['character::behaviour'], 'native field access changed')
                self.image.mem[byte] ^= 8 if byte == 0x130 else 1

    def test_optional_field_withholds_the_whole_group(self):
        self.image.mem[0x1B2] ^= 1
        before = copy.deepcopy(self.layout)
        result = self.result()
        self.assertTrue(result['ok'])
        self.assertEqual(result['disabled'], set(self.layout['groups']))
        self.assertTrue(all(result['rvas'][name] == 0 for name in self.layout['groups']))
        self.assertEqual(result['rvas']['playerBehavior'], 0x100)
        self.assertEqual(self.layout, before)

    def test_required_failure_is_not_hidden_by_optional_failure(self):
        self.image.mem[0x133] ^= 1
        self.image.mem[0x1B2] ^= 1
        before = copy.deepcopy(self.layout)
        result = self.result()
        self.assertFalse(result['ok'])
        self.assertIsNone(result['rvas'])
        self.assertEqual(len(result['why']), 2)
        self.assertEqual(self.layout, before)

    def test_owner_missing_ambiguous_or_disagreeing(self):
        before = bytes(self.image.mem)
        for why in ('native owner not found', 'native owner is ambiguous',
                    'native owner disagrees with resolved target'):
            with self.subTest(why=why):
                self.image.mem[:] = before
                self.layout['rvas']['playerBehavior'] = 0x100
                if why == 'native owner not found':
                    self.image.mem[0x100] = 0
                elif why == 'native owner is ambiguous':
                    self.image.mem[0x220:0x222] = b'\xaa\xbb'
                else:
                    self.layout['rvas']['playerBehavior'] = 0x110
                result = self.result()
                self.assertFalse(result['ok'])
                self.assertEqual(result['why']['character::behaviour'], why)

    def test_masks_allow_relocation_but_not_a_different_table(self):
        self.layout['rvas']['movementKeys'] += 0x20
        self.image.mem[0x153:0x157] = (0x320 + 0x90 - 0x157).to_bytes(4, 'little', signed=True)
        self.assertTrue(self.result()['ok'])
        self.image.mem[0x153] ^= 1
        result = self.result()
        self.assertFalse(result['ok'])
        self.assertEqual(result['why']['binding::interact'], 'native global-table reference changed')

    def test_missing_reference_target_and_bad_reference_bounds(self):
        self.layout['rvas']['movementKeys'] = 0
        self.assertFalse(self.result()['ok'])
        self.layout['rvas']['movementKeys'] = 0x300
        for key, value in [('displacement_offset', -1), ('displacement_offset', 5), ('next_offset', 8)]:
            with self.subTest(key=key, value=value):
                reference = self.spec['references'][0]
                saved = reference[key]
                reference[key] = value
                self.assertEqual(self.result()['why']['binding::interact'], 'invalid contract pattern')
                reference[key] = saved

    def test_unresolved_layout_and_bad_image(self):
        self.layout['ok'], self.layout['rvas'] = False, None
        self.assertFalse(self.result()['ok'])
        self.layout['ok'], self.layout['rvas'] = True, {}
        self.image.text = (0x100, 0x99999)
        self.assertFalse(self.result()['ok'])

    def test_bad_patterns_and_field_bounds(self):
        field = self.spec['fields'][0]
        pattern = field['pattern']
        for invalid in ('', '?? ??', 'GG', '0 11', 'aA BB'):
            with self.subTest(invalid=invalid):
                field['pattern'] = invalid
                self.assertEqual(self.result()['why'][field['name']], 'invalid contract pattern')
        field['pattern'] = pattern
        for offset in (-0x1000, 0x17A, 0xFFFFFFFF):
            field['window_offset'] = offset
            self.assertEqual(self.result()['why'][field['name']], 'native access is outside executable code')

    def test_unknown_optional_group_is_required(self):
        self.image.mem[0x1B2] ^= 1
        self.spec['fields'][1]['optional_group'] = 'typo'
        self.assertFalse(self.result()['ok'])


class ReviewedEvidenceTests(unittest.TestCase):
    def setUp(self):
        self.spec = json.loads(contracts.SPEC.read_text(encoding='utf-8'))
        self.offsets = contracts.field_offsets((contracts.ROOT / 'src/game_structs.h').read_text(encoding='utf-8'))

    def test_all_declarations_are_reviewed_and_header_matches(self):
        contracts.validate_spec(self.spec, self.offsets)
        self.assertEqual(contracts.HEADER.read_text(encoding='utf-8'), contracts.generated_header(self.spec))
        self.assertGreater(len(self.spec['fields']), 100)
        self.assertEqual(len(self.spec['references']), 10)

    def test_new_or_changed_field_requires_review(self):
        for name, value in [('character::notReviewed', 1234), ('character::headPart', 0x11E8)]:
            with self.subTest(name=name):
                changed = dict(self.offsets, **{name: value})
                with self.assertRaises(ValueError):
                    contracts.validate_spec(self.spec, changed)

    def test_missing_duplicate_or_unproven_field_is_rejected(self):
        for mutation in ('missing', 'duplicate', 'unproven'):
            spec = copy.deepcopy(self.spec)
            if mutation == 'missing':
                spec['fields'].pop()
            elif mutation == 'duplicate':
                spec['fields'].append(spec['fields'][0])
            else:
                spec['fields'][0]['provenance'] = ''
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                contracts.validate_spec(spec, self.offsets)

    def test_stride_derivation_requires_the_native_references(self):
        self.spec['references'][-1]['relative_add'] = 0xA0
        with self.assertRaises(ValueError):
            contracts.validate_spec(self.spec, self.offsets)

    def test_offsets_cannot_be_reclassified_as_mod_constants(self):
        field = self.spec['fields'].pop()
        field['kind'] = 'dimensional_constant'
        self.spec.setdefault('constants', []).append(field)
        with self.assertRaisesRegex(ValueError, 'Unreviewed mod constant'):
            contracts.validate_spec(self.spec, self.offsets)

    def test_aliases_must_reach_a_witness_without_cycles(self):
        owner = {'name': 'native', 'rva': 0x100, 'end_rva': 0x200}
        for derived, offsets in [
            ([{'name': 'node::pose', 'value': 0x40, 'base': 'node::pose', 'add': 0,
               'provenance': 'self reference is not native evidence'}], {'node::pose': 0x40}),
            ([{'name': 'node::pose', 'value': 0x40, 'base': 'node::origin', 'add': -0x24,
               'provenance': 'cyclic'},
              {'name': 'node::origin', 'value': 0x64, 'base': 'node::pose', 'add': 0x24,
               'provenance': 'cyclic'}], {'node::pose': 0x40, 'node::origin': 0x64}),
        ]:
            spec = {'schema': 1, 'owners': [owner], 'fields': [], 'derived': derived}
            with self.subTest(derived=derived), self.assertRaisesRegex(ValueError, 'Cyclic'):
                contracts.validate_spec(spec, offsets)

    def test_generator_rejects_wrong_owner_or_mid_instruction_rva(self):
        # A valid byte sequence in another function must not become an offset
        # from a different owner merely because its displacement looks right.
        for instruction in (0x101, 0x120):
            with self.subTest(instruction=instruction), tempfile.TemporaryDirectory() as folder:
                root = Path(folder)
                (root / 'src').mkdir()
                (root / 'src/game_structs.h').write_text(
                    'namespace node { inline constexpr size_t pose = 0x40; }', encoding='utf-8')
                executable = root / 'owned.bin'
                executable.write_bytes(b'owned fixture')
                spec = {'schema': 1, 'source_sha256': hashlib.sha256(executable.read_bytes()).hexdigest(),
                        'owners': [{'name': 'native', 'rva': 0x100, 'end_rva': 0x110}],
                        'fields': [{'name': 'node::pose', 'value': 0x40, 'owner': 'native',
                                    'instruction': instruction, 'width': 4, 'provenance': 'fixture'}]}
                source = root / 'native_contracts.json'
                source.write_text(json.dumps(spec), encoding='utf-8')
                exe = Mock()
                exe.text, exe.mem, exe.base = (0x100, 0x200), bytes(0x200), 0
                exe.unique_at.return_value = (b'\xaa\xbb', [1, 1])
                disassembler = Mock()
                disassembler.disasm.return_value = [Mock(address=0x100, size=2)]
                with patch.object(contracts, 'ROOT', root), patch.object(contracts, 'SPEC', source), \
                        patch.object(contracts.signatures, 'Exe', return_value=exe), \
                        patch.object(contracts.signatures, 'MD', disassembler), \
                        self.assertRaisesRegex(ValueError, 'not a boundary'):
                    contracts.generate(executable)


if __name__ == '__main__':
    unittest.main(verbosity=2)
