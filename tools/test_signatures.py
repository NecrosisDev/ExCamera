"""Checker contract tests using owned mapped-image fixtures; no Exanima or optional packages needed."""
import contextlib
import io
import json
import unittest
from unittest.mock import Mock, patch

import signatures


class Image:
    def __init__(self, slot=0x300):
        self.base = 0x140000000
        self.mem = bytearray(0x500)
        self.text = (0x100, 0x280)
        self.mem[0x100:0x102] = b'\xaa\xbb'
        self.mem[0x200:0x206] = b'\xcc\x00\x00\x00\x00\xdd'
        self.mem[0x201:0x205] = (slot - 0x205).to_bytes(4, 'little', signed=True)
        if slot + 8 <= len(self.mem):
            self.mem[slot:slot + 8] = (self.base + 0x100).to_bytes(8, 'little')

    def find(self, data, mask):
        return [at for at in range(self.text[0], self.text[1] - len(data) + 1)
                if all(not fixed or self.mem[at + i] == value
                       for i, (value, fixed) in enumerate(zip(data, mask)))][:3]


class CheckerTests(unittest.TestCase):
    rows = [
        ('navSampler', 'Code', False, 0, 0, 0, 0x100, 'AA BB'),
        ('navSlot', 'Relative', False, 1, 5, 0, 0x300, 'CC ?? ?? ?? ?? DD'),
    ]

    @staticmethod
    def header_text(rows, groups=None, slots=None):
        names = list(dict.fromkeys(row[0] for row in rows))
        groups = {} if groups is None else groups
        slots = [('navSlot', 'navSampler')] if slots is None else slots
        return (
            ''.join('Kind::' + row[1] + ', ' for row in rows)
            + 'targetNames[targetCount] = {\n' + ''.join('"%s",\n' % n for n in names) + '};\n'
            + 'targetOptionalGroups[targetCount] = {\n'
            + ''.join((json.dumps(groups[n]) if n in groups else 'nullptr') + ',\n' for n in names) + '};\n'
            + 'slotChecks[] = {\n' + ''.join('    {%s, %s},\n' % s for s in slots) + '};')

    def resolve(self, image, rows=None, groups=None, slots=None):
        rows = self.rows if rows is None else rows
        header = Mock()
        header.read_text.return_value = self.header_text(rows, groups, slots)
        output = io.StringIO()
        with patch.object(signatures, 'Exe', return_value=image), \
                patch.object(signatures, 'parse_header', return_value=rows), \
                patch.object(signatures, 'HEADER', header), contextlib.redirect_stdout(output):
            result = signatures.check('owned synthetic image')
        return result, output.getvalue()

    def test_valid_slot(self):
        self.assertEqual(self.resolve(Image())[0], 0)

    def test_slot_mismatch_fails(self):
        image = Image()
        image.mem[0x300:0x308] = (image.base + 0x120).to_bytes(8, 'little')
        result, output = self.resolve(image)
        self.assertEqual(result, 1)
        self.assertIn('slot holds something else', output)

    def test_truncated_slot_fails(self):
        result, output = self.resolve(Image(slot=0x4fc))
        self.assertEqual(result, 1)
        self.assertIn('outside the image', output)

    def test_outside_function_fails(self):
        image = Image()
        image.mem[0x300:0x308] = (image.base + 0x1100).to_bytes(8, 'little')
        rows = [('navSampler', 'Code', False, 0x1000, 0, 0, 0x1100, 'AA BB'), self.rows[1]]
        self.assertEqual(self.resolve(image, rows)[0], 1)

    def test_missing_entry_uses_slot(self):
        image = Image()
        image.mem[0x100:0x102] = b'\x90\x90'
        result, output = self.resolve(image)
        self.assertEqual(result, 0)
        self.assertIn('degraded', output)

    def test_conflicting_entries_are_not_rescued(self):
        image = Image()
        image.mem[0x110:0x112] = b'\xee\xff'
        rows = self.rows + [('navSampler', 'Code', False, 0, 0, 0, 0x100, 'EE FF')]
        result, output = self.resolve(image, rows)
        self.assertEqual(result, 1)
        self.assertIn('signatures disagree', output)

    def test_ambiguous_entries_are_not_rescued(self):
        image = Image()
        image.mem[0x110:0x112] = b'\xaa\xbb'
        result, output = self.resolve(image)
        self.assertEqual(result, 1)
        self.assertIn('more than one match', output)

    optional_rows = [
        ('core', 'Code', False, 0, 0, 0, 0x100, 'AA BB'),
        ('guardA', 'Code', True, 0, 0, 0, 0x140, 'EE 11'),
        ('guardB', 'Code', True, 0, 0, 0, 0x180, 'EE 22'),
    ]
    groups = {'guardA': 'manualDefense', 'guardB': 'manualDefense'}

    def optional_image(self):
        image = Image()
        image.mem[0x140:0x142] = b'\xee\x11'
        image.mem[0x180:0x182] = b'\xee\x22'
        return image

    def optional_resolution(self, image, rows=None):
        rows = self.optional_rows if rows is None else rows
        return signatures.resolve_rows(image, rows, self.header_text(rows, self.groups, []))

    def assert_withheld(self, result, why):
        self.assertTrue(result['ok'])
        self.assertEqual(result['rvas'], {'core': 0x100, 'guardA': 0, 'guardB': 0})
        self.assertEqual(result['why'], {'guardA': why})
        self.assertEqual(result['optional_failures'], 1)
        self.assertEqual(result['disabled'], {'guardA', 'guardB'})
        self.assertEqual(result['found']['guardB'], {0x180})

    def test_complete_optional_group(self):
        result = self.optional_resolution(self.optional_image())
        self.assertTrue(result['ok'])
        self.assertEqual(result['rvas'], {'core': 0x100, 'guardA': 0x140, 'guardB': 0x180})
        self.assertEqual(result['optional_failures'], 0)
        self.assertFalse(result['why'] or result['disabled'])

    def test_missing_optional_member_withholds_group(self):
        image = self.optional_image()
        image.mem[0x140:0x142] = b'\x90\x90'
        self.assert_withheld(self.optional_resolution(image), 'not found')
        exit_code, output = self.resolve(image, self.optional_rows, self.groups, [])
        self.assertEqual(exit_code, 0)
        self.assertIn('OPTIONAL', output)
        self.assertIn('disabled optional group manualDefense: guardA', output)
        self.assertIn('2 targets withheld by optional groups', output)
        self.assertNotIn('FAIL', output)

    def test_ambiguous_optional_member_withholds_group(self):
        image = self.optional_image()
        image.mem[0x150:0x152] = b'\xee\x11'
        self.assert_withheld(self.optional_resolution(image), 'more than one match')

    def test_conflicting_optional_member_withholds_group(self):
        image = self.optional_image()
        image.mem[0x160:0x162] = b'\xee\x33'
        rows = self.optional_rows + [('guardA', 'Code', True, 0, 0, 0, 0x140, 'EE 33')]
        self.assert_withheld(self.optional_resolution(image, rows), 'references disagree')

    def test_optional_bad_pattern_and_outside(self):
        for row, why in [
                (('guardA', 'Code', True, 0, 0, 0, 0x140, 'GG'), 'bad pattern'),
                (('guardA', 'Code', True, 0x1000, 0, 0, 0x140, 'EE 11'), 'outside the image')]:
            with self.subTest(why=why):
                rows = [self.optional_rows[0], row, self.optional_rows[2]]
                self.assert_withheld(self.optional_resolution(self.optional_image(), rows), why)

    def test_required_failure_never_hides_behind_optional_group(self):
        for lose_optional in (False, True):
            with self.subTest(lose_optional=lose_optional):
                image = self.optional_image()
                image.mem[0x100:0x102] = b'\x90\x90'
                if lose_optional:
                    image.mem[0x140:0x142] = b'\x90\x90'
                result = self.optional_resolution(image)
                self.assertFalse(result['ok'])
                self.assertIsNone(result['rvas'])
                self.assertEqual(result['optional_failures'], int(lose_optional))
                self.assertEqual(result['why']['core'], 'not found')
                self.assertEqual(self.resolve(image, self.optional_rows, self.groups, [])[0], 1)

    def test_slot_cannot_hide_optional_ambiguity_or_conflict(self):
        groups = {'navSampler': 'manualDefense', 'guardB': 'manualDefense'}
        for conflict in (False, True):
            with self.subTest(conflict=conflict):
                image = Image()
                image.mem[0x110:0x112] = b'\xee\xff' if conflict else b'\xaa\xbb'
                image.mem[0x180:0x182] = b'\xee\x22'
                rows = self.rows + [self.optional_rows[2]]
                if conflict:
                    rows += [('navSampler', 'Code', False, 0, 0, 0, 0x100, 'EE FF')]
                result = signatures.resolve_rows(image, rows, self.header_text(rows, groups))
                self.assertTrue(result['ok'])
                self.assertEqual(result['rvas'], {'navSampler': 0, 'navSlot': 0x300, 'guardB': 0})
                self.assertEqual(result['why']['navSampler'],
                                 'references disagree' if conflict else 'more than one match')

    def test_optional_metadata_generation_and_validation(self):
        spec = {'functions': {'guardA': {'optionalGroup': 'manualDefense'}},
                'returns': {}, 'globals': {}, 'slots': {}}
        self.assertEqual(signatures.optional_groups(spec), {'guardA': 'manualDefense'})
        for invalid in ('', None, 5, 'not a capability id'):
            spec['functions']['guardA']['optionalGroup'] = invalid
            with self.assertRaises(ValueError):
                signatures.optional_groups(spec)
        entries = [
            ('Code', 'core', 0x100, (b'\xaa\xbb', [1, 1]), 0, 0, 0, False, True),
            ('Code', 'guardA', 0x140, (b'\xee\x11', [1, 1]), 0, 0, 0, True, True),
        ]
        header = Mock()
        with patch.object(signatures, 'HEADER', header):
            signatures.write_header(entries, [], Image(), {'guardA': 'manualDefense'})
        text = header.write_text.call_args.args[0]
        self.assertEqual(signatures.parse_optional_groups(text, ['core', 'guardA']),
                         {'guardA': 'manualDefense'})
        with self.assertRaises(ValueError):
            signatures.parse_optional_groups(text, ['core'])


if __name__ == '__main__':
    unittest.main(verbosity=2)
