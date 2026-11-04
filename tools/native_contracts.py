"""Generate and check the reviewed native field contracts.

    python tools/native_contracts.py generate <source Exanima.exe>
    python tools/native_contracts.py check <Exanima.exe>

Generation validates and preserves all reviewed windows, including event witnesses,
interior owner anchors, dynamic field-offset bindings and optional call-graph closures,
using the archive identified in native_contracts.json. Checking any build uses its
instructions, call-graph closures and resolved targets, with no version/hash allowlist.
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

ALLOWED_OPTIONAL_GROUPS = (
    None,
    'manualDefense',
    'attackVariants',
    'physicsOwnership',
    'heldThrow',
    'settingsRedraw',
    'viewEffects',
    'nativeSettings',
    'inventoryReach',
    'headwearRender',
    'silhouette',
    'engineLog',
    'farRooms',
)

# Fixed C++ struct ABIs static_asserted in native_widgets.h / game_structs.h:
# layout changes in these structs must fail/withhold rather than mutate runtime offsets.
FIXED_ABI_AREAS = frozenset({'nativeElement', 'nativeString', 'meshRayQuery'})


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
                if area + '::' + name in result:
                    raise ValueError('Duplicate field declaration: ' + area + '::' + name)
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


def runtime_witnesses(spec):
    return ([field for field in spec['fields'] if 'witness' not in field] +
            spec.get('references', []) + spec.get('events', []))


def validate_closures(spec, owners, witnesses):
    closures = spec.get('closures')
    if closures is None:
        return
    callees = {}
    for callee in closures.get('callees', []):
        name = callee['name']
        if name in callees or not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*', name):
            raise ValueError('Invalid or duplicate closure callee: ' + str(name))
        pattern_bytes(callee['pattern'])
        if number(callee['rva']) <= 0:
            raise ValueError('Invalid closure callee RVA: ' + name)
        callees[name] = callee
    for branch in closures.get('branches', []):
        grp = branch.get('group')
        if grp not in ALLOWED_OPTIONAL_GROUPS or grp is None:
            raise ValueError('Invalid closure branch group: ' + str(grp))
        w = witnesses.get(branch.get('witness'))
        if w is None or w.get('optional_group') != grp:
            raise ValueError('Closure branch witness mismatch: ' + str(branch.get('witness')))
        w_len = len(w['pattern'].split())
        disp_off, insn_end = int(branch['disp_offset']), int(branch['insn_end'])
        if not (0 <= disp_off <= w_len - 4 and disp_off + 4 <= insn_end <= w_len):
            raise ValueError('Closure branch offset leaves witness: ' + w['name'])
        kind = branch.get('kind')
        if kind == 'intra':
            owner = owners[w['owner']]
            span = number(owner['end_rva']) - number(owner['rva'])
            if not 0 <= int(branch['owner_offset']) <= span:
                raise ValueError('Intra-owner closure offset leaves owner: ' + w['name'])
        elif kind == 'target':
            if not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*', branch.get('target', '')):
                raise ValueError('Invalid closure branch target: ' + w['name'])
        elif kind == 'owner':
            if branch.get('callee_owner') not in owners:
                raise ValueError('Missing closure callee owner: ' + str(branch.get('callee_owner')))
        elif kind == 'callee':
            if branch.get('callee') not in callees:
                raise ValueError('Missing closure callee: ' + str(branch.get('callee')))
        else:
            raise ValueError('Unknown closure branch kind: ' + str(kind))
    for slot in closures.get('vmt_slots', []):
        if slot.get('group') not in ALLOWED_OPTIONAL_GROUPS or slot.get('group') is None:
            raise ValueError('Invalid closure VMT slot group')
        if int(slot.get('slot_offset', 0)) < 0x60 or int(slot['slot_offset']) % 8 != 0:
            raise ValueError('Invalid closure VMT slot offset')
        kind = slot.get('kind')
        if kind == 'target':
            if not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*', slot.get('target', '')):
                raise ValueError('Invalid closure VMT target')
        elif kind == 'callee':
            if slot.get('callee') not in callees:
                raise ValueError('Missing closure VMT callee: ' + str(slot.get('callee')))
        else:
            raise ValueError('Unknown closure VMT slot kind: ' + str(kind))


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
        if field.get('optional_group') not in ALLOWED_OPTIONAL_GROUPS:
            raise ValueError('Unknown optional capability: ' + str(field.get('optional_group')))
    for event in spec.get('events', []):
        if event['owner'] not in owners:
            raise ValueError('Missing native event owner: ' + event['owner'])
        if event.get('optional_group') not in ALLOWED_OPTIONAL_GROUPS:
            raise ValueError('Unknown optional capability on event: ' + str(event.get('optional_group')))
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
    witness_map = {w['name']: w for w in runtime_witnesses(spec)}
    validate_closures(spec, owners, witness_map)


def generated_header(spec):
    lines = [
        '#pragma once',
        '// Generated by tools/native_contracts.py; provenance is in tools/native_contracts.json.',
        '#include "game_structs.h"',
        '#include <array>',
    ]

    # 1. Runtime-adaptable live offsets (fp::game::live), initialized from constexpr fp::game defaults.
    areas = {}
    area_order = []
    constant_names = {c['name'] for c in spec.get('constants', [])}
    mutable_names = []
    for item in spec['fields'] + spec.get('derived', []) + spec.get('constants', []):
        area, member = item['name'].split('::', 1)
        if area not in areas:
            areas[area] = []
            area_order.append(area)
        is_const = item['name'] in constant_names or area in FIXED_ABI_AREAS
        areas[area].append((member, item['name'], is_const))
        if not is_const:
            mutable_names.append(item['name'])

    lines.append('namespace fp::game::live {')
    for area in area_order:
        lines.append('namespace %s {' % area)
        for member, full_name, is_const in areas[area]:
            qual = 'inline constexpr size_t' if is_const else 'inline size_t'
            lines.append('    %s %s = fp::game::%s;' % (qual, member, full_name))
        lines.append('}')
    lines.append('inline void recomputeDerivedOffsets() noexcept {')
    for d in spec.get('derived', []):
        d_area = d['name'].split('::', 1)[0]
        if 'base' in d and d_area not in FIXED_ABI_AREAS:
            mult = number(d.get('multiply', 1))
            add = number(d.get('add', 0))
            lines.append('    %s = %s * %du + %du;' % (d['name'], d['base'], mult, add))
    lines.append('}')
    lines.append('inline void resetLiveOffsets() noexcept {')
    for full_name in mutable_names:
        lines.append('    %s = fp::game::%s;' % (full_name, full_name))
    lines.append('    recomputeDerivedOffsets();')
    lines.append('}')
    lines.append('} // namespace fp::game::live')

    # 2. Contract owners and runtime witnesses
    lines += ['namespace fp::contract {', 'inline constexpr ContractOwner owners[] = {']
    indices = {owner['name']: i for i, owner in enumerate(spec['owners'])}
    owners_by_name = {owner['name']: owner for owner in spec['owners']}
    for owner in spec['owners']:
        target = 'sig::' + owner.get('target', 'targetCount')
        lines.append('    {%s, %s, %s, %d},' % (json.dumps(owner['name']), target,
                                               json.dumps(owner['pattern']), owner['entry_offset']))
    lines += ['};', 'inline constexpr FieldContract fields[] = {']
    witness_list = runtime_witnesses(spec)
    witness_indices = {w['name']: i for i, w in enumerate(witness_list)}
    witness_by_name = {w['name']: w for w in witness_list}
    for field in witness_list:
        group = json.dumps(field['optional_group']) if field.get('optional_group') else 'nullptr'
        reference = ', sig::%s, %d, %d, %d, %d' % (field['relative_to'], field['displacement_offset'],
                                                   field['next_offset'], number(field['relative_add']), field['width']) if 'relative_to' in field else ''
        lines.append('    {%s, %d, 0x%X, %s, %s%s},' % (json.dumps(field['name']), indices[field['owner']],
                                                       field['window_offset'], json.dumps(field['pattern']), group, reference))
    lines.append('};')

    # 3. Dynamic struct-displacement bindings for hde64 extraction
    binding_rows = []
    for field in spec['fields']:
        area = field['name'].split('::', 1)[0]
        val = number(field['value'])
        if field.get('operand_kind') == 'immediate' or val == 0 or area in FIXED_ABI_AREAS:
            continue
        w_name = field.get('witness', field['name'])
        w_idx = witness_indices[w_name]
        w = witness_by_name[w_name]
        o_rva = number(owners_by_name[field['owner']]['rva'])
        insn_off = number(field['instruction']) - (o_rva + w['window_offset'])
        adj = number(field.get('adjust', 0))
        exp_disp = val + adj
        negated = (exp_disp == -val)
        if adj != 0 and not negated:
            continue
        binding_rows.append('    {%du, %du, %d, %s, game::%s, &game::live::%s},' % (
            w_idx, insn_off, exp_disp, 'true' if negated else 'false', field['name'], field['name']))
    if binding_rows:
        lines.append('inline constexpr std::array<FieldOffsetBinding, %d> fieldBindings = {{' % len(binding_rows))
        lines += binding_rows
        lines.append('}};')
    else:
        lines.append('inline constexpr std::array<FieldOffsetBinding, 0> fieldBindings = {};')

    # 4. Call-graph closure verification tables
    closures = spec.get('closures') or {}
    callees = closures.get('callees', [])
    callee_indices = {c['name']: i for i, c in enumerate(callees)}
    if callees:
        lines.append('inline constexpr std::array<ClosureCallee, %d> closureCallees = {{' % len(callees))
        for c in callees:
            lines.append('    {%s, %s},' % (json.dumps(c['name']), json.dumps(c['pattern'])))
        lines.append('}};')
    else:
        lines.append('inline constexpr std::array<ClosureCallee, 0> closureCallees = {};')

    branches = closures.get('branches', [])
    if branches:
        lines.append('inline constexpr std::array<ClosureBranch, %d> closureBranches = {{' % len(branches))
        for b in branches:
            w_idx = witness_indices[b['witness']]
            kind = b['kind']
            if kind == 'intra':
                arg = str(int(b['owner_offset']))
            elif kind == 'target':
                arg = 'static_cast<int32_t>(sig::%s)' % b['target']
            elif kind == 'owner':
                arg = str(indices[b['callee_owner']])
            else:
                arg = str(callee_indices[b['callee']])
            lines.append('    {%s, %du, %du, %du, ClosureKind::%s, %s},' % (
                json.dumps(b['group']), w_idx, int(b['disp_offset']), int(b['insn_end']), kind, arg))
        lines.append('}};')
    else:
        lines.append('inline constexpr std::array<ClosureBranch, 0> closureBranches = {};')

    vmt_slots = closures.get('vmt_slots', [])
    if vmt_slots:
        lines.append('inline constexpr std::array<ClosureVmtSlot, %d> closureVmtSlots = {{' % len(vmt_slots))
        for s in vmt_slots:
            kind = s['kind']
            arg = ('static_cast<uint32_t>(sig::%s)' % s['target']) if kind == 'target' else ('%du' % callee_indices[s['callee']])
            lines.append('    {%s, sig::%s, 0x%Xu, ClosureKind::%s, %s},' % (
                json.dumps(s['group']), s['vmt_target'], int(s['slot_offset']), kind, arg))
        lines.append('}};')
    else:
        lines.append('inline constexpr std::array<ClosureVmtSlot, 0> closureVmtSlots = {};')

    for field in spec['fields'] + spec.get('derived', []) + spec.get('constants', []):
        lines.append('static_assert(game::%s == 0x%X, "Review native contract after changing %s");' % (
            field['name'], number(field['value']), field['name']))
    lines += ['} // namespace fp::contract', '']
    return '\n'.join(lines)


def verify_closures_on_exe(exe, spec, targets):
    closures = spec.get('closures')
    if not closures:
        return
    owners_by_name = {o['name']: o for o in spec['owners']}
    witness_by_name = {w['name']: w for w in runtime_witnesses(spec)}
    func_targets = {k: int(v['rva'], 16) for k, v in targets['functions'].items()}
    glob_targets = {k: int(v['rva'], 16) for k, v in targets['globals'].items()}
    callees = {c['name']: c for c in closures.get('callees', [])}

    for c in callees.values():
        rva = number(c['rva'])
        data, mask = pattern_bytes(c['pattern'])
        if not exe.text[0] <= rva <= exe.text[1] - len(data):
            raise ValueError('Closure callee leaves .text: ' + c['name'])
        if any(fixed and exe.mem[rva + i] != val for i, (val, fixed) in enumerate(zip(data, mask))):
            raise ValueError('Closure callee pattern differs at source RVA: ' + c['name'])

    for b in closures.get('branches', []):
        w = witness_by_name[b['witness']]
        owner = owners_by_name[w['owner']]
        at = number(owner['rva']) + w['window_offset']
        rel = int.from_bytes(exe.mem[at + b['disp_offset']:at + b['disp_offset'] + 4], 'little', signed=True)
        dest = at + b['insn_end'] + rel
        if b['kind'] == 'intra':
            expected = number(owner['rva']) + int(b['owner_offset'])
        elif b['kind'] == 'target':
            expected = func_targets[b['target']]
        elif b['kind'] == 'owner':
            expected = number(owners_by_name[b['callee_owner']]['rva'])
        else:
            expected = number(callees[b['callee']]['rva'])
        if dest != expected:
            raise ValueError('Closure branch destination mismatch in %s: 0x%X != 0x%X' % (b['witness'], dest, expected))

    for s in closures.get('vmt_slots', []):
        vmt_rva = glob_targets[s['vmt_target']]
        va = int.from_bytes(exe.mem[vmt_rva + s['slot_offset']:vmt_rva + s['slot_offset'] + 8], 'little')
        dest = va - exe.base
        expected = func_targets[s['target']] if s['kind'] == 'target' else number(callees[s['callee']]['rva'])
        if dest != expected:
            raise ValueError('Closure VMT slot mismatch at +0x%X: 0x%X != 0x%X' % (s['slot_offset'], dest, expected))


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
    # Reviewed windows are part of the source specification, not disposable
    # generator output. Preserve their masks, offsets and event coverage exactly.
    instructions = {}
    witnesses = {field['name']: field for field in runtime_witnesses(spec)}
    for owner in spec['owners']:
        begin, end = number(owner['rva']), number(owner['end_rva'])
        if not exe.text[0] <= begin < end <= exe.text[1]:
            raise ValueError('Reviewed owner is outside executable code: ' + owner['name'])
        code = list(signatures.MD.disasm(bytes(exe.mem[begin:end]), exe.base + begin))
        for field in spec['fields'] + spec.get('references', []):
            if field['owner'] == owner['name'] and not any(i.address - exe.base == number(field['instruction']) for i in code):
                raise ValueError('Field instruction is not a boundary in its reviewed owner: ' + field['name'])
        data, mask = pattern_bytes(owner['pattern'])
        anchor = begin - owner['entry_offset']
        if not begin <= anchor < anchor + len(data) <= end:
            raise ValueError('Reviewed owner anchor leaves its native range: ' + owner['name'])
        if exe.find(data, mask) != [anchor]:
            raise ValueError('Reviewed owner pattern is not unique: ' + owner['name'])
        instructions[owner['name']] = code
    for field in runtime_witnesses(spec):
        owner = owner_specs[field['owner']]
        start = number(owner['rva']) + field['window_offset']
        data, mask = pattern_bytes(field['pattern'])
        if not number(owner['rva']) <= start < start + len(data) <= number(owner['end_rva']):
            raise ValueError('Contract window leaves its reviewed owner: ' + field['name'])
        if any(fixed and exe.mem[start + i] != value for i, (value, fixed) in enumerate(zip(data, mask))):
            raise ValueError('Reviewed contract window differs: ' + field['name'])
    for field in spec['fields'] + spec.get('references', []):
        owner = owner_specs[field['owner']]
        insn = next((i for i in instructions[field['owner']]
                     if i.address - exe.base == number(field['instruction'])), None)
        if insn is None:
            raise ValueError('Field instruction is not a boundary in its reviewed owner: ' + field['name'])
        witness = witnesses[field.get('witness', field['name'])]
        start = number(owner['rva']) + witness['window_offset']
        if witness['owner'] != field['owner'] or not start <= insn.address - exe.base < insn.address - exe.base + insn.size <= start + len(witness['pattern'].split()):
            raise ValueError('Contract window does not contain its field access: ' + field['name'])
        if 'relative_to' in field:
            target = number(globals_by_name[field['relative_to']]['rva']) + number(field['relative_add'])
            matching = [op for op in insn.operands if op.type == signatures.x86.X86_OP_MEM and
                        op.mem.base == signatures.x86.X86_REG_RIP and op.size == field['width'] and
                        insn.address - exe.base + insn.size + op.mem.disp == target]
            if insn.disp_size != 4:
                raise ValueError('Global reference must use rel32: ' + field['name'])
            if field['displacement_offset'] != insn.address - exe.base - start + insn.disp_offset or field['next_offset'] != insn.address - exe.base - start + insn.size:
                raise ValueError('Global reference encoding differs: ' + field['name'])
        elif field.get('operand_kind') == 'immediate':
            matching = [op for op in insn.operands if op.type == signatures.x86.X86_OP_IMM and
                        op.imm == number(field['value']) and op.size == field['width']]
        else:
            displacement = number(field['value']) + number(field.get('adjust', 0))
            matching = [op for op in insn.operands if op.type == signatures.x86.X86_OP_MEM and
                        op.mem.base not in (0, signatures.x86.X86_REG_RIP, signatures.x86.X86_REG_RSP) and
                        op.mem.disp == displacement and op.size == field['width']]
        if not matching:
            raise ValueError('Reviewed field instruction differs: ' + field['name'])
    verify_closures_on_exe(exe, spec, targets)
    HEADER.write_text(generated_header(spec), encoding='utf-8', newline='\n')
    print('GENERATED %d runtime witnesses, %d native owners; verified %d field declarations' % (
        len(runtime_witnesses(spec)), len(spec['owners']), len(spec['fields'])))


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
    witnesses = runtime_witnesses(spec)
    witness_by_name = {w['name']: w for w in witnesses}
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

    closures = spec.get('closures') or {}
    callees = {c['name']: pattern_bytes(c['pattern']) for c in closures.get('callees', [])}
    closure_failed_groups = set()
    if layout['ok']:
        for b in closures.get('branches', []):
            grp = b['group']
            if grp in closure_failed_groups:
                continue
            w = witness_by_name[b['witness']]
            entry, ow_why = owners[w['owner']]
            if ow_why is not None or b['witness'] in failures:
                closure_failed_groups.add(grp)
                continue
            at = entry + w['window_offset']
            rel = int.from_bytes(exe.mem[at + b['disp_offset']:at + b['disp_offset'] + 4], 'little', signed=True)
            dest = at + b['insn_end'] + rel
            ok_branch = exe.text[0] <= dest < exe.text[1]
            if ok_branch:
                if b['kind'] == 'intra':
                    ok_branch = (dest == entry + int(b['owner_offset']))
                elif b['kind'] == 'target':
                    t_rva = rvas.get(b['target'], 0)
                    ok_branch = (t_rva != 0 and dest == t_rva)
                elif b['kind'] == 'owner':
                    c_entry, c_why = owners[b['callee_owner']]
                    ok_branch = (c_why is None and dest == c_entry)
                else:
                    c_data, c_mask = callees[b['callee']]
                    ok_branch = (dest + len(c_data) <= exe.text[1] and
                                 all(not fixed or exe.mem[dest + i] == val
                                     for i, (val, fixed) in enumerate(zip(c_data, c_mask))))
            if not ok_branch:
                closure_failed_groups.add(grp)
                failures.setdefault('closure::' + grp, 'optional call-graph closure changed')
        for s in closures.get('vmt_slots', []):
            grp = s['group']
            if grp in closure_failed_groups:
                continue
            vmt_rva = rvas.get(s['vmt_target'], 0)
            off = int(s['slot_offset'])
            if not vmt_rva or vmt_rva + off + 8 > len(exe.mem):
                closure_failed_groups.add(grp)
                failures.setdefault('closure::' + grp, 'optional VMT slot outside image')
                continue
            va = int.from_bytes(exe.mem[vmt_rva + off:vmt_rva + off + 8], 'little')
            dest = va - exe.base
            ok_slot = exe.text[0] <= dest < exe.text[1]
            if ok_slot:
                if s['kind'] == 'target':
                    t_rva = rvas.get(s['target'], 0)
                    ok_slot = (t_rva != 0 and dest == t_rva)
                else:
                    c_data, c_mask = callees[s['callee']]
                    ok_slot = (dest + len(c_data) <= exe.text[1] and
                               all(not fixed or exe.mem[dest + i] == val
                                   for i, (val, fixed) in enumerate(zip(c_data, c_mask))))
            if not ok_slot:
                closure_failed_groups.add(grp)
                failures.setdefault('closure::' + grp, 'optional VMT slot changed')
    for grp in closure_failed_groups:
        disabled |= {name for name, declared in layout.get('groups', {}).items() if declared == grp}

    for name in disabled:
        rvas[name] = 0
    return {'ok': layout['ok'] and not required_failure, 'why': failures, 'disabled': disabled,
            'rvas': None if required_failure else rvas, 'checked': len(witnesses),
            'closure_failed_groups': closure_failed_groups}


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
