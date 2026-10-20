"""Check native source specifications and generated headers without a game executable.

Run: python tools/check_generated.py
Only the Python standard library is required. This detects source/header drift;
it does not replace native executable validation with the two generators/checkers.
"""
import json
import sys
from types import SimpleNamespace

import native_contracts as contracts
import signatures


def check():
    spec = json.loads(contracts.SPEC.read_text(encoding='utf-8'))
    targets = json.loads(signatures.TARGETS.read_text(encoding='utf-8'))
    offsets = contracts.field_offsets((contracts.ROOT / 'src/game_structs.h').read_text(encoding='utf-8'))
    contracts.validate_spec(spec, offsets)
    entries = signatures.reviewed_entries(targets)
    if targets['source_sha256'] != spec['source_sha256']:
        raise ValueError('Native specifications identify different reviewed source builds')
    owners = {owner['name']: owner for owner in spec['owners']}
    witnesses = {field['name']: field for field in contracts.runtime_witnesses(spec)}
    if len(witnesses) != len(contracts.runtime_witnesses(spec)):
        raise ValueError('Duplicate runtime witness')
    for owner in owners.values():
        contracts.pattern_bytes(owner['pattern'])
        if owner.get('target') and owner['target'] not in targets['target_order']:
            raise ValueError('Native owner target is undeclared: ' + owner['name'])
    for field in witnesses.values():
        contracts.pattern_bytes(field['pattern'])
        if field['owner'] not in owners or not field.get('provenance'):
            raise ValueError('Incomplete runtime witness: ' + field['name'])
        owner = owners[field['owner']]
        if field['window_offset'] < 0 or field['window_offset'] + len(field['pattern'].split()) > contracts.number(owner['end_rva']) - contracts.number(owner['rva']):
            raise ValueError('Runtime witness leaves owner: ' + field['name'])
    for field in spec['fields']:
        witness = witnesses.get(field.get('witness', field['name']))
        if not witness or witness['owner'] != field['owner']:
            raise ValueError('Field has no matching runtime witness: ' + field['name'])
        instruction = contracts.number(field['instruction']) - contracts.number(owners[field['owner']]['rva'])
        if not witness['window_offset'] <= instruction < witness['window_offset'] + len(witness['pattern'].split()):
            raise ValueError('Field instruction leaves runtime witness: ' + field['name'])
    expected = signatures.generated_header(entries, [(n, t['holds']) for n, t in targets['slots'].items()],
                                            SimpleNamespace(base=int(targets['source_image_base'], 16)),
                                            signatures.optional_groups(targets), targets['target_order'])
    if signatures.HEADER.read_text(encoding='utf-8') != expected:
        raise ValueError('Generated signature header is stale; review targets.json and regenerate')
    if contracts.HEADER.read_text(encoding='utf-8') != contracts.generated_header(spec):
        raise ValueError('Generated native-contract header is stale; review native_contracts.json and regenerate')
    print('%d targets / %d signatures; %d owners / %d runtime witnesses; %d field declarations: consistent' % (
        len(targets['target_order']), len(entries), len(owners), len(witnesses), len(offsets)))


if __name__ == '__main__':
    try:
        check()
    except (ValueError, KeyError, IndexError) as error:
        print('Generated source consistency: ' + str(error), file=sys.stderr)
        sys.exit(1)
