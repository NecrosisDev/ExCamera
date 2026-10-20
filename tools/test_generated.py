"""Source/header drift checks use no game executable or disassembly packages."""
import copy
import json
import unittest
from unittest.mock import patch

import check_generated
import native_contracts
import signatures


class GeneratedSourceTests(unittest.TestCase):
    def test_duplicate_field_declarations_are_rejected(self):
        source = ('namespace sample { inline constexpr size_t member = 4; }\n'
                  'namespace sample { inline constexpr size_t member = 4; }')
        with self.assertRaisesRegex(ValueError, 'Duplicate field declaration'):
            native_contracts.field_offsets(source)

    def test_checked_in_sources_are_consistent(self):
        check_generated.check()

    def test_shared_physics_dependency_is_not_owned_by_either_feature(self):
        targets = json.loads(signatures.TARGETS.read_text(encoding='utf-8'))
        groups = signatures.optional_groups(targets)
        for name in ('nativePhysicsThread', 'nativePhysicsContext', 'nativePhysicsScene'):
            self.assertEqual(groups[name], 'physicsOwnership')
        self.assertEqual(groups['attackFamilyA'], 'attackVariants')
        self.assertEqual(groups['heldBodyIntegrate'], 'heldThrow')
        spec = json.loads(native_contracts.SPEC.read_text(encoding='utf-8'))
        common = [w for w in native_contracts.runtime_witnesses(spec)
                  if w.get('optional_group') == 'physicsOwnership']
        self.assertEqual(len(common), 10)

    def test_signature_header_drift_is_rejected(self):
        with patch.object(signatures, 'HEADER') as header:
            header.read_text.return_value = '// stale'
            with self.assertRaisesRegex(ValueError, 'signature header is stale'):
                check_generated.check()

    def test_native_header_drift_is_rejected(self):
        with patch.object(native_contracts, 'HEADER') as header:
            header.read_text.return_value = '// stale'
            with self.assertRaisesRegex(ValueError, 'native-contract header is stale'):
                check_generated.check()

    def test_missing_event_witness_is_rejected(self):
        spec = json.loads(native_contracts.SPEC.read_text(encoding='utf-8'))
        spec['events'] = [e for e in spec['events'] if e['name'] != 'nativeForm::scrollCoordinateLayout']
        with patch.object(native_contracts, 'SPEC') as source:
            source.read_text.return_value = json.dumps(spec)
            with self.assertRaisesRegex(ValueError, 'no matching runtime witness'):
                check_generated.check()

    def test_signature_metadata_drift_is_rejected(self):
        original = json.loads(signatures.TARGETS.read_text(encoding='utf-8'))
        for key, value in [('known_rva', '0x1'), ('hook', False), ('offset', -1)]:
            spec = copy.deepcopy(original)
            row = next(r for r in spec['reviewed_signatures'] if r['hook'])
            row[key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                signatures.reviewed_entries(spec)

    def test_missing_target_or_signature_is_rejected(self):
        spec = json.loads(signatures.TARGETS.read_text(encoding='utf-8'))
        first = spec['reviewed_signatures'][0]['target']
        spec['reviewed_signatures'] = [r for r in spec['reviewed_signatures'] if r['target'] != first]
        with self.assertRaisesRegex(ValueError, 'Targets without reviewed signatures'):
            signatures.reviewed_entries(spec)


if __name__ == '__main__':
    unittest.main()
