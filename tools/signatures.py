"""Code signatures for every Exanima address ExCamera uses.

    python tools/signatures.py generate <Exanima.exe>   write game_signatures.h from tools/targets.json
    python tools/signatures.py check <Exanima.exe>      resolve game_signatures.h against an exe and report

`generate` needs the exe the RVAs in targets.json belong to. `check` is what to run on a new game build:
it lists every target that no longer resolves uniquely and every one that moved. A required target
failure refuses the layout; one optional-group failure withholds its entire capability while retaining
the valid core layout. Needs: pip install pefile capstone.

A pattern keeps opcodes, registers, struct-field displacements and immediates, and wildcards only what
moves when code is relinked (rel32 branch targets, RIP-relative displacements, absolute image addresses).
So a match is the same instruction sequence touching the same struct fields as in the build it came from.
"""
from pathlib import Path
import argparse
import json
import re
import sys

ROOT = Path(__file__).resolve().parent.parent
TARGETS = ROOT / 'tools' / 'targets.json'
# The public repository keeps sources in src/; the development tree keeps them beside tools/.
HEADER = (ROOT / 'src' if (ROOT / 'src').is_dir() else ROOT) / 'game_signatures.h'
MIN_BYTES, MAX_BYTES = 24, 160
MD = None


class Exe:
    def __init__(self, path):
        # The checker fixtures use a mapped image directly. PE/disassembly
        # dependencies are needed only when an actual executable is opened.
        global capstone, x86, MD
        import capstone
        import pefile
        from capstone import x86
        if MD is None:
            MD = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
            MD.detail = True
        self.pe = pefile.PE(str(path), fast_load=False)
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.mem = self.pe.get_memory_mapped_image()
        text = next(s for s in self.pe.sections if s.Name.rstrip(b'\0') == b'.text')
        self.text = (text.VirtualAddress, text.VirtualAddress + text.Misc_VirtualSize)
        self.text_bytes = bytes(self.mem[self.text[0]:self.text[1]])
        self.funcs = sorted((e.struct.BeginAddress, e.struct.EndAddress)
                            for e in getattr(self.pe, 'DIRECTORY_ENTRY_EXCEPTION', []))
        self._refs = None

    # ---- masking ----
    def masked(self, insn, blind=False):
        """blind: also wildcard the register choice (REX prefix, ModRM and SIB bytes), keeping opcodes, struct
        displacements and immediates, so a recompile that picks other registers still matches."""
        raw = bytes(insn.bytes)
        mask = [1] * len(raw)
        if blind:
            for k in range(len(raw)):
                if 0x40 <= raw[k] <= 0x4F and k < (insn.modrm_offset or len(raw)) and k < 4:
                    mask[k] = 0   # a REX prefix before the opcode
            if insn.modrm_offset:
                mask[insn.modrm_offset] = 0
                modrm = raw[insn.modrm_offset]
                if (modrm >> 6) != 3 and (modrm & 7) == 4 and insn.modrm_offset + 1 < len(raw):
                    mask[insn.modrm_offset + 1] = 0   # the SIB byte
        lo, hi = self.base, self.base + len(self.mem)
        if insn.disp_size:
            for op in insn.operands:
                if op.type != x86.X86_OP_MEM:
                    continue
                absolute = op.mem.base == 0 and op.mem.index == 0 and lo <= (op.mem.disp & (2**64 - 1)) < hi
                if op.mem.base == x86.X86_REG_RIP or absolute:
                    mask[insn.disp_offset:insn.disp_offset + insn.disp_size] = [0] * insn.disp_size
        if insn.imm_size:
            branch = insn.group(capstone.CS_GRP_JUMP) or insn.group(capstone.CS_GRP_CALL)
            absolute = any(op.type == x86.X86_OP_IMM and lo <= (op.imm & (2**64 - 1)) < hi for op in insn.operands)
            if (branch and insn.imm_size >= 4) or (not branch and absolute):
                mask[insn.imm_offset:insn.imm_offset + insn.imm_size] = [0] * insn.imm_size
        return raw, mask

    def insns_from(self, rva, count=48):
        return list(MD.disasm(bytes(self.mem[rva:rva + 16 * count]), self.base + rva, count))

    def window(self, rva, min_bytes, cover_to=None, blind=False):
        data, mask = b'', []
        for i in self.insns_from(rva):
            b, m = self.masked(i, blind)
            data += b
            mask += m
            if len(data) >= min_bytes and (cover_to is None or rva + len(data) >= cover_to):
                break
        return data, mask

    def find(self, data, mask, limit=3):
        rx = re.compile(b''.join(re.escape(bytes([b])) if m else b'.' for b, m in zip(data, mask)), re.DOTALL)
        hits, pos = [], 0
        while len(hits) < limit:
            m = rx.search(self.text_bytes, pos)
            if not m:
                break
            hits.append(self.text[0] + m.start())
            pos = m.start() + 1
        return hits

    def unique_at(self, rva, cover_to=None, blind=False):
        for size in range(MIN_BYTES, MAX_BYTES + 1, 8):
            data, mask = self.window(rva, size, cover_to, blind)
            if self.find(data, mask) == [rva]:
                return data, mask
        return None

    def boundaries_before(self, rva):
        """Instruction starts in the function containing rva, up to rva (from .pdata)."""
        f = next(((a, b) for a, b in self.funcs if a <= rva < b), None)
        if not f:
            return []
        starts = [i.address - self.base for i in MD.disasm(bytes(self.mem[f[0]:rva]), self.base + f[0])]
        return [s for s in starts if s < rva]

    def calls_to(self, target):
        """(call insn rva, its function) for every direct call (E8 rel32) to target."""
        if getattr(self, '_calls', None) is None:
            self._calls = {}
            t0 = self.text[0]
            b = self.text_bytes
            for k in range(len(b) - 5):
                if b[k] == 0xE8:
                    dest = t0 + k + 5 + int.from_bytes(b[k + 1:k + 5], 'little', signed=True)
                    self._calls.setdefault(dest, []).append(t0 + k)
        out = []
        for c in self._calls.get(target, []):
            f = next(((a, z) for a, z in self.funcs if a <= c < z), None)
            if f and c != target:
                out.append((c, f[0]))
        return out

    def refs(self, target):
        if self._refs is None:
            self._refs = {}
            for a, b in self.funcs:
                if not (self.text[0] <= a < self.text[1]):
                    continue
                for i in MD.disasm(bytes(self.mem[a:b]), self.base + a):
                    for op in i.operands:
                        if op.type == x86.X86_OP_MEM and op.mem.base == x86.X86_REG_RIP:
                            t = i.address + i.size + op.mem.disp - self.base
                            self._refs.setdefault(t, []).append((i.address - self.base, i.disp_offset, i.size, a))
        return self._refs.get(target, [])


def text_pattern(data, mask):
    return ' '.join('%02X' % b if m else '??' for b, m in zip(data, mask))


def code_signature(exe, start, anchor, blind=False):
    """Unique pattern containing anchor (start..), else starting up to 4 instructions earlier."""
    candidates = [start] + list(reversed(exe.boundaries_before(start)))[:4]
    for s in candidates:
        found = exe.unique_at(s, cover_to=anchor + 1, blind=blind)
        if found:
            return s, found
    return None, None


def add_unique(out, entry):
    """Keeps a signature only if its pattern is new for that target (a blind copy equal to the exact one adds nothing)."""
    name, pattern = entry[1], text_pattern(*entry[3])
    if not any(o[1] == name and text_pattern(*o[3]) == pattern for o in out):
        out.append(entry)


def optional_groups(spec):
    """Per-target optional capability IDs; absence means the target is required."""
    groups = {}
    for table in ('functions', 'returns', 'globals', 'slots'):
        for name, target in spec[table].items():
            if 'optionalGroup' not in target:
                continue
            group = target['optionalGroup']
            if not isinstance(group, str) or not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*', group):
                raise ValueError('%s optionalGroup must be a nonempty capability identifier' % name)
            groups[name] = group
    return groups


def generate(exe_path):
    """Several independent signatures per target, so one edited function or reference does not lose it:
    functions by their own first bytes (exact and register-blind) and by up to two callers' call instructions;
    return sites by the call around them (exact and blind); globals and vtable slots by two references in
    different functions (each exact and blind)."""
    exe = Exe(exe_path)
    spec = json.loads(TARGETS.read_text(encoding='utf-8'))
    groups = optional_groups(spec)
    out, failed = [], []
    for name, t in spec['functions'].items():
        rva = int(t['rva'], 16)
        before = len(out)
        for blind in (False, True):
            s, found = code_signature(exe, rva, rva, blind)
            if found and s == rva:
                add_unique(out, ('Code', name, rva, found, 0, 0, 0, t.get('hook', False), True))
        made, used = 0, set()
        for call, func in exe.calls_to(rva):
            if func in used or made == 2:
                continue
            s, found = code_signature(exe, call, call + 4)
            if found:
                add_unique(out, ('Relative', name, rva, found, call - s + 1, call - s + 5, 0, t.get('hook', False), False))
                used.add(func)
                made += 1
        made, used = 0, set()
        for insn, disp_off, size, func in sorted(exe.refs(rva), key=lambda r: r[0]):
            if func in used or made == 2:
                continue
            s, found = code_signature(exe, insn, insn + size - 1)
            if found:
                add_unique(out, ('Relative', name, rva, found, insn - s + disp_off, insn - s + size, 0, t.get('hook', False), False))
                used.add(func)
                made += 1
        if len(out) == before:
            failed.append(name)
    for name, t in spec['returns'].items():
        rva = int(t['rva'], 16)
        call = next((i for i in reversed(exe.boundaries_before(rva))), None)
        before = len(out)
        for blind in (False, True):
            s, found = code_signature(exe, call, rva, blind) if call is not None else (None, None)
            if found:
                add_unique(out, ('Code', name, rva, found, rva - s, 0, 0, False, False))
            # what follows the call: no byte in common with the call's window
            after = exe.unique_at(rva, blind=blind)
            if after:
                add_unique(out, ('Code', name, rva, after, 0, 0, 0, False, False))
        if len(out) == before:
            failed.append(name)
    for kind, table in (('global', spec['globals']), ('slot', spec['slots'])):
        for name, t in table.items():
            target = int(t['vtable' if kind == 'slot' else 'rva'], 16)
            addend = int(t.get('offset', '0'), 16)
            before, used = len(out), set()
            for insn, disp_off, size, func in sorted(exe.refs(target), key=lambda r: r[0]):
                if func in used or len(used) == 2:
                    continue
                made = False
                for blind in (False, True):
                    s, found = code_signature(exe, insn, insn + size - 1, blind)
                    if found:
                        add_unique(out, ('Relative', name, target + addend, found, insn - s + disp_off, insn - s + size, addend, False, False))
                        made = True
                if made:
                    used.add(func)
            if len(out) == before:
                failed.append(name)
    if failed:
        sys.exit('No unique signature for: ' + ', '.join(failed))
    slots = [(n, t['holds']) for n, t in spec['slots'].items()]
    write_header(out, slots, exe, groups)
    per = {}
    for o in out:
        per[o[1]] = per.get(o[1], 0) + 1
    single = sorted(n for n, c in per.items() if c == 1)
    print('WROTE', HEADER, '(%d signatures for %d targets; %d with only one: %s)' % (
        len(out), len(per), len(single), ', '.join(single) or 'none'))


def write_header(entries, slots, exe, groups=None):
    groups = {} if groups is None else groups
    names = []
    for e in entries:
        if e[1] not in names:
            names.append(e[1])
    lines = [
        '// Generated by tools/signatures.py from tools/targets.json. Do not edit by hand.',
        '// clang-format off',
        '// Source build: Exanima 0.9.5.2g (Steam build 25803178), image base 0x%X.' % exe.base,
        '#pragma once',
        '#include <cstdint>',
        'namespace fp::sig {',
        'enum class Kind : uint8_t { Code, Relative };',
        'enum Target : uint16_t {',
    ]
    lines += ['    %s,' % n for n in names]
    lines += ['    targetCount', '};', 'inline constexpr const char* targetNames[targetCount] = {']
    lines += ['    "%s",' % n for n in names]
    lines += ['};', '// A missing member withholds every address in its optional capability group.',
              'inline constexpr const char* targetOptionalGroups[targetCount] = {']
    lines += ['    %s,' % (json.dumps(groups[n]) if n in groups else 'nullptr') for n in names]
    lines += ['};',
              '// Code: resolves to match + offset. Relative: to match + insnEnd + int32 at (match + offset) + addend (a RIP-relative',
              '// reference to a global or a vtable, or a call to the function).',
              'struct Signature {', '    Target target;', '    Kind kind;', '    bool hook;',
              '    bool entry; // matches at the function\'s first byte (compared with the file for other programs\' detours)',
              '    uint16_t offset;',
              '    uint16_t insnEnd;', '    int32_t addend;', '    uint32_t knownRva; // in the source build',
              '    const char* pattern;', '};', 'inline constexpr Signature signatures[] = {']
    for kind, name, rva, (data, mask), off, end, addend, hook, entry in entries:
        lines.append('    {%s, Kind::%s, %s, %s, %d, %d, %d, 0x%X, "%s"},' % (
            name, kind, 'true' if hook else 'false', 'true' if entry else 'false', off, end, addend, rva, text_pattern(data, mask)))
    lines += ['};', '// A slot must hold the resolved function: proof the vtable and the function belong together.',
              'struct SlotCheck {', '    Target slot, holds;', '};', 'inline constexpr SlotCheck slotChecks[] = {']
    lines += ['    {%s, %s},' % s for s in slots]
    lines += ['};', '} // namespace fp::sig', '// clang-format on', '']
    HEADER.write_text('\n'.join(lines), encoding='utf-8', newline='\n')


def parse_header():
    text = HEADER.read_text(encoding='utf-8')
    rows = re.findall(r'\{(\w+), Kind::(\w+), (\w+), (\w+), (\d+), (\d+), (-?\d+), 0x([0-9A-F]+), "([0-9A-F? ]+)"\}', text)
    return [(n, k, h == 'true', int(o), int(e), int(a), int(r, 16), p) for n, k, h, _entry, o, e, a, r, p in rows]


def parse_optional_groups(text, names):
    """Read the generated per-target metadata, never a potentially newer targets.json."""
    declaration = re.search(r'targetOptionalGroups\[targetCount\]\s*=\s*\{([^}]*)\}', text)
    if declaration is None:
        return {}  # older required-only headers remain readable
    tokens = [value.strip() for value in declaration[1].split(',') if value.strip()]
    if len(tokens) != len(names):
        raise ValueError('optional-group metadata does not match the target count')
    groups = {}
    for name, token in zip(names, tokens):
        if token == 'nullptr':
            continue
        group = json.loads(token)
        if not isinstance(group, str) or not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*', group):
            raise ValueError('invalid optional-group identifier for ' + name)
        groups[name] = group
    return groups


def resolve_rows(exe, rows, text):
    """The runtime resolver's target/group policy, separated from CLI formatting for contract tests."""
    found, missed, miss_why = {}, {}, {}
    known = {row[0]: row[6] for row in rows}
    names = re.search(r'targetNames\[targetCount\]\s*=\s*\{([^}]*)\}', text)
    names = re.findall(r'"(\w+)"', names[1]) if names else list(known)
    if len(names) != len(known) or set(names) != set(known):
        raise ValueError('target names do not match parsed signatures')
    groups = parse_optional_groups(text, names)

    def miss(name, why):
        missed[name] = missed.get(name, 0) + 1
        if miss_why.get(name, 'not found') == 'not found':
            miss_why[name] = why

    for name, kind, hook, off, end, addend, rva_known, pattern in rows:
        known[name] = rva_known
        tokens = pattern.split()
        if not tokens or not any(b != '??' for b in tokens) or \
                any(not re.fullmatch(r'[0-9A-F]{2}|\?\?', b) for b in tokens):
            miss(name, 'bad pattern')
            continue
        data = bytes(int(b, 16) if b != '??' else 0 for b in tokens)
        mask = [b != '??' for b in tokens]
        hits = exe.find(data, mask)
        if len(hits) != 1:
            miss(name, 'more than one match' if hits else 'not found')
            continue
        at = hits[0]
        if kind == 'Code':
            rva = at + off
        else:
            if not 0 <= at + off <= len(exe.mem) - 4:
                miss(name, 'outside the image')
                continue
            rva = at + end + int.from_bytes(exe.mem[at + off:at + off + 4], 'little', signed=True) + addend
        if not 0 <= rva < len(exe.mem) or (rva == 0 and name in groups):
            miss(name, 'outside the image')
            continue
        found.setdefault(name, set()).add(rva)
    # a function reached only through a vtable: its slot holds its address (as the DLL does)
    holds = dict(re.findall(r'\{(\w+), (\w+)\},', text.split('slotChecks[]', 1)[1].split('};', 1)[0]))
    for slot, func in holds.items():
        if func not in found and miss_why.get(func, 'not found') == 'not found' and \
                slot in found and len(found[slot]) == 1:
            at = next(iter(found[slot]))
            if not 0 <= at <= len(exe.mem) - 8:
                continue
            held = int.from_bytes(exe.mem[at:at + 8], 'little') - exe.base
            if exe.text[0] <= held < exe.text[1]:
                found[func] = {held}
                missed[func] = missed.get(func, 0) or 1
    slot_failures = {}
    for slot, func in holds.items():
        if len(found.get(slot, ())) != 1 or len(found.get(func, ())) != 1:
            continue
        at = next(iter(found[slot]))
        if not 0 <= at <= len(exe.mem) - 8:
            slot_failures[slot] = 'outside the image'
        elif int.from_bytes(exe.mem[at:at + 8], 'little') != exe.base + next(iter(found[func])):
            slot_failures[slot] = 'slot holds something else'
    why = {n: miss_why.get(n, 'not found') for n in known if n not in found}
    why.update((n, 'references disagree') for n, values in found.items() if len(values) > 1)
    why.update(slot_failures)
    failed_groups = {groups[n] for n in why if n in groups}
    disabled = {n for n, group in groups.items() if group in failed_groups}
    optional_failures = sum(n in groups for n in why)
    ok = len(why) == optional_failures
    # A rejected core has no usable new layout. On success every member of an
    # unavailable optional group is explicitly zero, just as in Layout.rva.
    rvas = {n: 0 if n in disabled else next(iter(found[n])) for n in known} if ok else None
    return dict(ok=ok, rvas=rvas, found=found, why=why, known=known, groups=groups,
                disabled=disabled, optional_failures=optional_failures,
                degraded={n for n in found if n in missed and n not in why}, missed=missed)


def check(exe_path):
    """Required failures reject the layout; an optional failure withholds its complete group."""
    exe = Exe(exe_path)
    rows = parse_header()
    text = HEADER.read_text(encoding='utf-8')
    declared = text.count('Kind::Code, ') + text.count('Kind::Relative, ')
    if not rows or len(rows) != declared:
        sys.exit('Could not read %s (%d of %d signatures parsed); regenerate it.' % (HEADER, len(rows), declared))
    try:
        result = resolve_rows(exe, rows, text)
    except (ValueError, KeyError, IndexError) as error:
        sys.exit('Could not read %s: %s; regenerate it.' % (HEADER, error))
    for name in sorted(result['found']):
        if name in result['why'] or name in result['disabled']:
            continue
        at = next(iter(result['found'][name]))
        if at != result['known'][name]:
            print('moved     %-24s 0x%X -> 0x%X' % (name, result['known'][name], at))
    for name in sorted(result['degraded']):
        print('degraded  %-24s %d of its signatures no longer match' % (name, result['missed'][name]))
    for name, why in sorted(result['why'].items()):
        label = 'OPTIONAL' if name in result['groups'] else 'FAIL'
        if why == 'references disagree':
            why = 'signatures disagree: ' + ', '.join('0x%X' % at for at in sorted(result['found'][name]))
        print('%-9s %-24s %s' % (label, name, why))
    for group in sorted({result['groups'][n] for n in result['disabled']}):
        failures = sorted(n for n in result['why'] if result['groups'].get(n) == group)
        print('disabled optional group %s: %s' % (group, ', '.join(failures)))
    print('%d targets resolved (%d degraded), %d lost; %d optional failures, %d targets withheld by optional groups' % (
        len(result['known']) - len(result['why']), len(result['degraded']), len(result['why']),
        result['optional_failures'], len(result['disabled'])))
    return 0 if result['ok'] else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('command', choices=['generate', 'check'])
    parser.add_argument('exe', type=Path)
    args = parser.parse_args()
    if args.command == 'generate':
        generate(args.exe)
    else:
        sys.exit(check(args.exe))


if __name__ == '__main__':
    main()
