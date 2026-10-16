"""Code signatures for every Exanima address ExCamera uses.

    python tools/signatures.py generate <Exanima.exe>   write game_signatures.h from tools/targets.json
    python tools/signatures.py check <Exanima.exe>      resolve game_signatures.h against an exe and report

`generate` needs the exe the RVAs in targets.json belong to. `check` is what to run on a new game build:
it lists every target that no longer resolves uniquely (the DLL refuses to start in that case) and every
one that moved. Needs: pip install pefile capstone.

A pattern keeps opcodes, registers, struct-field displacements and immediates, and wildcards only what
moves when code is relinked (rel32 branch targets, RIP-relative displacements, absolute image addresses).
So a match is the same instruction sequence touching the same struct fields as in the build it came from.
"""
from pathlib import Path
import argparse
import json
import re
import sys

import capstone
import pefile
from capstone import x86

ROOT = Path(__file__).resolve().parent.parent
TARGETS = ROOT / 'tools' / 'targets.json'
# The public repository keeps sources in src/; the development tree keeps them beside tools/.
HEADER = (ROOT / 'src' if (ROOT / 'src').is_dir() else ROOT) / 'game_signatures.h'
MIN_BYTES, MAX_BYTES = 24, 160
MD = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
MD.detail = True


class Exe:
    def __init__(self, path):
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
    def masked(self, insn):
        raw = bytes(insn.bytes)
        mask = [1] * len(raw)
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

    def window(self, rva, min_bytes, cover_to=None):
        data, mask = b'', []
        for i in self.insns_from(rva):
            b, m = self.masked(i)
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

    def unique_at(self, rva, cover_to=None):
        for size in range(MIN_BYTES, MAX_BYTES + 1, 8):
            data, mask = self.window(rva, size, cover_to)
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


def code_signature(exe, start, anchor):
    """Unique pattern containing anchor (start..), else starting up to 4 instructions earlier."""
    candidates = [start] + list(reversed(exe.boundaries_before(start)))[:4]
    for s in candidates:
        found = exe.unique_at(s, cover_to=anchor + 1)
        if found:
            return s, found
    return None, None


def generate(exe_path):
    exe = Exe(exe_path)
    spec = json.loads(TARGETS.read_text(encoding='utf-8'))
    out, failed = [], []
    function_rvas = {}
    for name, t in spec['functions'].items():
        rva = int(t['rva'], 16)
        function_rvas[name] = rva
        s, found = code_signature(exe, rva, rva)
        if not found or s != rva:
            failed.append(name)
            continue
        out.append(('Code', name, rva, found, 0, 0, 0, t.get('hook', False)))
    for name, t in spec['returns'].items():
        rva = int(t['rva'], 16)
        call = next((i for i in reversed(exe.boundaries_before(rva))), None)
        s, found = code_signature(exe, call, rva) if call is not None else (None, None)
        if not found:
            failed.append(name)
            continue
        out.append(('Code', name, rva, found, rva - s, 0, 0, False))
    for kind, table in (('global', spec['globals']), ('slot', spec['slots'])):
        for name, t in table.items():
            target = int(t['vtable' if kind == 'slot' else 'rva'], 16)
            addend = int(t.get('offset', '0'), 16)
            made, used_funcs = 0, set()
            for insn, disp_off, size, func in sorted(exe.refs(target), key=lambda r: r[0]):
                if func in used_funcs:
                    continue  # independent references come from different functions
                s, found = code_signature(exe, insn, insn + size - 1)
                if not found:
                    continue
                out.append(('Data', name, target + addend, found, insn - s + disp_off, insn - s + size, addend, False))
                used_funcs.add(func)
                made += 1
                if made == 2:
                    break
            if not made:
                failed.append(name)
    if failed:
        sys.exit('No unique signature for: ' + ', '.join(failed))
    slots = [(n, t['holds']) for n, t in spec['slots'].items()]
    write_header(out, slots, exe)
    print('WROTE', HEADER, '(%d signatures for %d targets)' % (len(out), len({o[1] for o in out})))


def write_header(entries, slots, exe):
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
        'enum class Kind : uint8_t { Code, Data };',
        'enum Target : uint16_t {',
    ]
    lines += ['    %s,' % n for n in names]
    lines += ['    targetCount', '};', 'inline constexpr const char* targetNames[targetCount] = {']
    lines += ['    "%s",' % n for n in names]
    lines += ['};',
              '// Code: resolves to match + offset. Data: to match + insnEnd + int32 at (match + offset) + addend.',
              'struct Signature {', '    Target target;', '    Kind kind;', '    bool hook;', '    uint16_t offset;',
              '    uint16_t insnEnd;', '    int32_t addend;', '    uint32_t knownRva; // in the source build',
              '    const char* pattern;', '};', 'inline constexpr Signature signatures[] = {']
    for kind, name, rva, (data, mask), off, end, addend, hook in entries:
        lines.append('    {%s, Kind::%s, %s, %d, %d, %d, 0x%X, "%s"},' % (
            name, kind, 'true' if hook else 'false', off, end, addend, rva, text_pattern(data, mask)))
    lines += ['};', '// A slot must hold the resolved function: proof the vtable and the function belong together.',
              'struct SlotCheck {', '    Target slot, holds;', '};', 'inline constexpr SlotCheck slotChecks[] = {']
    lines += ['    {%s, %s},' % s for s in slots]
    lines += ['};', '} // namespace fp::sig', '// clang-format on', '']
    HEADER.write_text('\n'.join(lines), encoding='utf-8', newline='\n')


def parse_header():
    text = HEADER.read_text(encoding='utf-8')
    rows = re.findall(r'\{(\w+), Kind::(\w+), (\w+), (\d+), (\d+), (-?\d+), 0x([0-9A-F]+), "([0-9A-F? ]+)"\}', text)
    return [(n, k, h == 'true', int(o), int(e), int(a), int(r, 16), p) for n, k, h, o, e, a, r, p in rows]


def check(exe_path):
    exe = Exe(exe_path)
    resolved, problems = {}, []
    rows = parse_header()
    declared = HEADER.read_text(encoding='utf-8').count('Kind::Code, ') + HEADER.read_text(encoding='utf-8').count('Kind::Data, ')
    if not rows or len(rows) != declared:
        sys.exit('Could not read %s (%d of %d signatures parsed); regenerate it.' % (HEADER, len(rows), declared))
    for name, kind, hook, off, end, addend, known, pattern in rows:
        data = bytes(int(b, 16) if b != '??' else 0 for b in pattern.split())
        mask = [b != '??' for b in pattern.split()]
        hits = exe.find(data, mask)
        if len(hits) != 1:
            problems.append('%-24s %s' % (name, 'not found' if not hits else 'ambiguous (%d+ matches)' % len(hits)))
            continue
        at = hits[0]
        if kind == 'Code':
            rva = at + off
        else:
            disp = int.from_bytes(exe.mem[at + off:at + off + 4], 'little', signed=True)
            rva = at + end + disp + addend
        if name in resolved and resolved[name] != rva:
            problems.append('%-24s references disagree (0x%X vs 0x%X)' % (name, resolved[name], rva))
        resolved[name] = rva
        if rva != known:
            print('moved  %-24s 0x%X -> 0x%X' % (name, known, rva))
    for line in problems:
        print('FAIL   ' + line)
    print('%d targets resolved, %d problems' % (len(resolved), len(problems)))
    return 1 if problems else 0


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
