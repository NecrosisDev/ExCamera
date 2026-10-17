"""Generate and check the reviewed native field contracts.

    python tools/native_contracts.py generate <source Exanima.exe>
    python tools/native_contracts.py check <Exanima.exe>

Generation requires the archive identified in native_contracts.json. Checking any
build uses its instructions and resolved targets, with no version/hash allowlist.
"""
from pathlib import Path
import argparse
import ast
import hashlib
import json
import re
import sys

import signatures

ROOT = Path(__file__).resolve().parent.parent
SPEC = ROOT / 'tools/native_contracts.json'
HEADER = ROOT / 'src/game_contracts.h'


def field_offsets(text):
    source = re.sub(r'//[^\n]*', '', text)
    result = {}
    for match in re.finditer(r'namespace (\w+)\s*\{(.*?)\}', source, re.S):
        area, body = match.groups()
        names = {}

        def value(node):
            if isinstance(node, ast.Constant) and type(node.value) is int:
                return node.value
            if isinstance(node, ast.Name):
                return names[node.id]
            if isinstance(node, ast.BinOp) and isinstance(node.op, (ast.Add, ast.Mult)):
                a, b = value(node.left), value(node.right)
                return a + b if isinstance(node.op, ast.Add) else a * b
            raise ValueError('Unsupported field expression: ' + ast.dump(node))

        for declaration in re.findall(r'inline constexpr size_t\s+(.*?);', body, re.S):
            for item in declaration.split(','):
                name, expression = item.strip().split('=', 1)
                name = name.strip()
                names[name] = value(ast.parse(expression.strip(), mode='eval').body)
                result[area + '::' + name] = names[name]
    return result


def number(value):
    return int(value, 0) if isinstance(value, str) else int(value)


def pattern_bytes(text):
    tokens = text.split()
    if not tokens or not any(token != '??' for token in tokens) or any(
            not re.fullmatch(r'[0-9A-F]{2}|\?\?', token) for token in tokens):
        raise ValueError('invalid contract pattern')
    return bytes(0 if token == '??' else int(token, 16) for token in tokens), [token != '??' for token in tokens]


def validate_spec(spec, offsets):
    if spec.get('schema') != 1:
        raise ValueError('Unsupported native-contract schema')
    covered = {}
    owners = {owner['name']: owner for owner in spec['owners']}
    if len(owners) != len(spec['owners']):
        raise ValueError('Duplicate native owner')
    for owner in owners.values():
        if not 0 <= number(owner['rva']) < number(owner['end_rva']):
            raise ValueError('Invalid reviewed owner range: ' + owner['name'])
    for field in spec['fields'] + spec.get('derived', []) + spec.get('constants', []):
        name = field['name']
        if name in covered or name not in offsets or number(field['value']) != offsets[name]:
            raise ValueError('Field declaration differs from reviewed contract: ' + name)
        if not field.get('provenance'):
            raise ValueError('Missing field provenance: ' + name)
        covered[name] = field
    for field in spec['fields']:
        if field['owner'] not in owners:
            raise ValueError('Missing native owner: ' + field['owner'])
        if field.get('optional_group') not in (None, 'manualDefense'):
            raise ValueError('Unknown optional capability')
    references = {field['name']: field for field in spec.get('references', [])}
    if len(references) != len(spec.get('references', [])):
        raise ValueError('Duplicate global reference')
    for field in references.values():
        if field['owner'] not in owners or not field.get('provenance') or not field.get('relative_to'):
            raise ValueError('Incomplete global reference: ' + field['name'])
    for field in spec.get('derived', []):
        if 'reference_base' in field:
            refs = [references[name] for name in field['reference_names']]
            if len(refs) < 2 or any(ref['relative_to'] != field['reference_base'] or
                                   number(ref['relative_add']) != index * offsets[field['name']]
                                   for index, ref in enumerate(refs)):
                raise ValueError('Invalid global stride evidence: ' + field['name'])
            continue
        base = field['base']
        if base not in covered or offsets[base] * number(field.get('multiply', 1)) + number(field.get('add', 0)) != offsets[field['name']]:
            raise ValueError('Invalid derived field: ' + field['name'])
    witnessed = {field['name'] for field in spec['fields']}
    # This is a mod loop bound, not a native offset. Do not let arbitrary fields
    # bypass native evidence by relabeling them as constants.
    for field in spec.get('constants', []):
        if field['name'] != 'bindings::count' or field.get('kind') != 'mod_scan_bound' or number(field['value']) != len(references):
            raise ValueError('Unreviewed mod constant: ' + field['name'])
    complete = set(witnessed)
    visiting = set()

    def trace(name):
        if name in complete:
            return
        if name in visiting:
            raise ValueError('Cyclic field derivation: ' + name)
        visiting.add(name)
        field = covered[name]
        if 'reference_base' not in field:
            if 'base' not in field:
                raise ValueError('Derived field has no native evidence: ' + name)
            trace(field['base'])
        visiting.remove(name)
        complete.add(name)

    for field in spec.get('derived', []):
        trace(field['name'])
    missing = sorted(set(offsets) - set(covered))
    if missing:
        raise ValueError('Fields without reviewed contracts: ' + ', '.join(missing))


def generated_header(spec):
    lines = ['#pragma once', '// Generated by tools/native_contracts.py; provenance is in tools/native_contracts.json.',
             '#include "game_structs.h"', 'namespace fp::contract {', 'inline constexpr ContractOwner owners[] = {']
    indices = {owner['name']: i for i, owner in enumerate(spec['owners'])}
    for owner in spec['owners']:
        target = 'sig::' + owner.get('target', 'targetCount')
        lines.append('    {%s, %s, %s, %d},' % (json.dumps(owner['name']), target,
                                               json.dumps(owner['pattern']), owner['entry_offset']))
    lines += ['};', 'inline constexpr FieldContract fields[] = {']
    for field in spec['fields'] + spec.get('references', []):
        group = json.dumps(field['optional_group']) if field.get('optional_group') else 'nullptr'
        reference = ', sig::%s, %d, %d, %d, %d' % (field['relative_to'], field['displacement_offset'],
                                                   field['next_offset'], number(field['relative_add']), field['width']) if 'relative_to' in field else ''
        lines.append('    {%s, %d, 0x%X, %s, %s%s},' % (json.dumps(field['name']), indices[field['owner']],
                                                       field['window_offset'], json.dumps(field['pattern']), group, reference))
    lines.append('};')
    for field in spec['fields'] + spec.get('derived', []) + spec.get('constants', []):
        lines.append('static_assert(game::%s == 0x%X, "Review native contract after changing %s");' % (
            field['name'], number(field['value']), field['name']))
    lines += ['} // namespace fp::contract', '']
    return '\n'.join(lines)


def generate(path):
    spec = json.loads(SPEC.read_text(encoding='utf-8'))
    offsets = field_offsets((ROOT / 'src/game_structs.h').read_text(encoding='utf-8'))
    validate_spec(spec, offsets)
    if hashlib.sha256(path.read_bytes()).hexdigest() != spec['source_sha256']:
        raise ValueError('Generation requires the reviewed source archive; update provenance before regenerating')
    exe = signatures.Exe(path)
    targets = json.loads(signatures.TARGETS.read_text(encoding='utf-8'))
    globals_by_name = targets['globals']
    owner_specs = {owner['name']: owner for owner in spec['owners']}
    instructions = {}
    for owner in spec['owners']:
        begin, end = number(owner['rva']), number(owner['end_rva'])
        if not exe.text[0] <= begin < end <= exe.text[1]:
            raise ValueError('Reviewed owner is outside executable code: ' + owner['name'])
        code = list(signatures.MD.disasm(bytes(exe.mem[begin:end]), exe.base + begin))
        instructions[owner['name']] = code
        unique = exe.unique_at(begin)
        if not unique or begin + len(unique[0]) > end:
            raise ValueError('No unique native-owner entry: ' + owner['name'])
        owner['pattern'] = signatures.text_pattern(*unique)
        owner['entry_offset'] = 0
    for field in spec['fields'] + spec.get('references', []):
        owner = owner_specs[field['owner']]
        begin, instruction = number(owner['rva']), number(field['instruction'])
        code = instructions[owner['name']]
        index = next((i for i, insn in enumerate(code) if insn.address - exe.base == instruction), None)
        if index is None:
            raise ValueError('Field instruction is not a boundary in its reviewed owner: ' + field['name'])
        insn = code[index]
        if 'relative_to' in field:
            target = number(globals_by_name[field['relative_to']]['rva']) + number(field['relative_add'])
            matching = [op for op in insn.operands if op.type == signatures.x86.X86_OP_MEM and
                        op.mem.base == signatures.x86.X86_REG_RIP and op.size == field['width'] and
                        insn.address - exe.base + insn.size + op.mem.disp == target]
        else:
            displacement = number(field['value']) + number(field.get('adjust', 0))
            matching = [op for op in insn.operands if op.type == signatures.x86.X86_OP_MEM and
                        op.mem.base not in (0, signatures.x86.X86_REG_RIP, signatures.x86.X86_REG_RSP) and
                        op.mem.disp == displacement and op.size == field['width']]
        if not matching:
            raise ValueError('Reviewed field instruction differs: ' + field['name'])
        window = code[max(0, index - 2):]
        start = window[0].address - exe.base
        data, mask = b'', []
        for current in window:
            raw, fixed = exe.masked(current)
            data += raw
            mask += fixed
            if len(data) >= 24 and start + len(data) >= instruction + insn.size:
                break
        if not (start <= instruction and instruction + insn.size <= start + len(data)):
            raise ValueError('Contract window does not contain its field access')
        if not begin <= start < start + len(data) <= number(owner['end_rva']):
            raise ValueError('Contract window leaves its reviewed owner')
        field['window_offset'] = start - begin
        field['pattern'] = signatures.text_pattern(data, mask)
        field['native_instruction'] = insn.mnemonic + ' ' + insn.op_str
        field['displacement_offset'] = instruction - start + insn.disp_offset
        field['displacement_bytes'] = insn.disp_size
        if 'relative_to' in field:
            if insn.disp_size != 4:
                raise ValueError('Global reference must use rel32: ' + field['name'])
            field['next_offset'] = instruction - start + insn.size
    SPEC.write_text(json.dumps(spec, indent=2) + '\n', encoding='utf-8')
    HEADER.write_text(generated_header(spec), encoding='utf-8')
    print('GENERATED %d field witnesses, %d global references, %d native owners, %d derived/constants' % (
        len(spec['fields']), len(spec.get('references', [])), len(spec['owners']),
        len(spec.get('derived', [])) + len(spec.get('constants', []))))


def resolve_contracts(exe, spec, layout):
    owners = {}
    for owner in spec['owners']:
        why, entry = None, None
        if not layout['ok']:
            why = 'game targets are unresolved'
        elif not 0 <= exe.text[0] < exe.text[1] <= len(exe.mem):
            why = 'native access is outside executable code'
        else:
            try:
                data, mask = pattern_bytes(owner['pattern'])
                found = exe.find(data, mask)
                if len(found) != 1:
                    why = 'native owner is ambiguous' if found else 'native owner not found'
                else:
                    entry = found[0] + owner['entry_offset']
                    if not exe.text[0] <= entry < exe.text[1]:
                        why = 'native access is outside executable code'
                    elif owner.get('target') and layout['rvas'].get(owner['target'], 0) not in (0, entry):
                        why = 'native owner disagrees with resolved target'
            except ValueError:
                why = 'invalid contract pattern'
        owners[owner['name']] = entry, why
    failures, disabled = {}, set()
    rvas = dict(layout['rvas'] or {})
    required_failure = False
    witnesses = spec['fields'] + spec.get('references', [])
    for field in witnesses:
        entry, why = owners[field['owner']]
        if why is None:
            try:
                data, mask = pattern_bytes(field['pattern'])
                at = entry + field['window_offset']
                if not exe.text[0] <= at <= exe.text[1] - len(data):
                    why = 'native access is outside executable code'
                elif any(fixed and exe.mem[at + i] != value for i, (value, fixed) in enumerate(zip(data, mask))):
                    why = 'native field access changed'
                elif 'relative_to' in field:
                    disp, after = field['displacement_offset'], field['next_offset']
                    if not 0 <= disp <= len(data) - 4 or not 0 <= after <= len(data):
                        why = 'invalid contract pattern'
                    else:
                        relative = int.from_bytes(exe.mem[at + disp:at + disp + 4], 'little', signed=True)
                        target = at + after + relative
                        expected = layout['rvas'].get(field['relative_to'], 0)
                        width = field.get('width', 1)
                        if not expected or width <= 0 or not 0 <= target <= len(exe.mem) - width or target != expected + number(field['relative_add']):
                            why = 'native global-table reference changed'
            except ValueError:
                why = 'invalid contract pattern'
        if why:
            failures[field['name']] = why
            group = field.get('optional_group')
            members = {name for name, declared in layout.get('groups', {}).items() if group and declared == group}
            if members:
                disabled |= members
            else:
                required_failure = True
    for name in disabled:
        rvas[name] = 0
    return {'ok': layout['ok'] and not required_failure, 'why': failures, 'disabled': disabled,
            'rvas': None if required_failure else rvas, 'checked': len(witnesses)}


def check(path):
    spec = json.loads(SPEC.read_text(encoding='utf-8'))
    validate_spec(spec, field_offsets((ROOT / 'src/game_structs.h').read_text(encoding='utf-8')))
    if HEADER.read_text(encoding='utf-8') != generated_header(spec):
        raise ValueError('Generated native-contract header is stale')
    exe = signatures.Exe(path)
    layout = signatures.resolve_rows(exe, signatures.parse_header(), signatures.HEADER.read_text(encoding='utf-8'))
    result = resolve_contracts(exe, spec, layout)
    for name, why in result['why'].items():
        print('FIELD %-38s %s' % (name, why))
    print('%d native field/reference contracts checked, %d failed, %d optional targets withheld' % (
        result['checked'], len(result['why']), len(result['disabled'])))
    return 0 if result['ok'] else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('command', choices=['generate', 'check'])
    parser.add_argument('exe', type=Path)
    args = parser.parse_args()
    try:
        return generate(args.exe) if args.command == 'generate' else check(args.exe)
    except (ValueError, KeyError, IndexError) as error:
        print('Native contracts: ' + str(error), file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
