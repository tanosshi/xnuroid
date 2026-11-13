#!/usr/bin/env python3
"""Static view of a classic-dyld armv7 image, for work that does not have the device attached.

Decodes the same LC_DYLD_INFO streams the loader applies, classifies every bind by the section that owns its slot
(lazy pointer = function, everything else = data), and walks ARM code with capstone following direct calls and calls
through bound slots.

Usage:
    python tools/armv7_static_trace.py build/v7probe/Calculator32
    python tools/armv7_static_trace.py build/v7probe/Calculator32 --from _globalinit_33_..._func5 --depth 3
"""
import argparse
import struct
import sys

try:
    import capstone
except ImportError:  # the tool stays usable for the census without it
    capstone = None

MH_MAGIC = 0xfeedface
LC_SEGMENT = 0x1
LC_SYMTAB = 0x2
LC_DYLD_INFO = 0x22
LC_DYLD_INFO_ONLY = 0x80000022
LC_MAIN = 0x80000028
LC_UNIXTHREAD = 0x5

BIND_OPCODE_MASK = 0xF0
BIND_IMMEDIATE_MASK = 0x0F
BIND_OPCODE_DONE = 0x00
BIND_OPCODE_SET_DYLIB_ORDINAL_IMM = 0x10
BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB = 0x20
BIND_OPCODE_SET_DYLIB_SPECIAL_IMM = 0x30
BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM = 0x40
BIND_OPCODE_SET_TYPE_IMM = 0x50
BIND_OPCODE_SET_ADDEND_SLEB = 0x60
BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB = 0x70
BIND_OPCODE_ADD_ADDR_ULEB = 0x80
BIND_OPCODE_DO_BIND = 0x90
BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB = 0xA0
BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED = 0xB0
BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB = 0xC0


def u32(d, o):
    return struct.unpack_from('<I', d, o)[0]


def cstr(d, o):
    return d[o:d.index(b'\0', o)].decode('utf-8', 'replace')


def uleb(d, o):
    v = shift = 0
    while True:
        b = d[o]
        o += 1
        v |= (b & 0x7F) << shift
        shift += 7
        if not b & 0x80:
            return v, o


def sleb(d, o):
    v = shift = 0
    while True:
        b = d[o]
        o += 1
        v |= (b & 0x7F) << shift
        shift += 7
        if not b & 0x80:
            return v - (1 << shift) if b & 0x40 else v, o


class Image:
    def __init__(self, path):
        with open(path, 'rb') as f:
            self.d = d = f.read()
        if u32(d, 0) != MH_MAGIC:
            raise SystemExit(f'{path}: not a thin 32-bit Mach-O')
        self.cputype, self.subtype, self.filetype = struct.unpack_from('<III', d, 4)
        self.segments, self.sections = [], []
        self.entry, self.entry_thumb = 0, False
        self.symtab = self.dyld = None
        off = 28   # mach_header is 28 bytes for 32-bit images
        for _ in range(u32(d, 16)):
            cmd, cmdsize = struct.unpack_from('<II', d, off)
            base = cmd & 0x7FFFFFFF
            if base == LC_SEGMENT:
                nsects = u32(d, off + 48)
                self.segments.append((cstr(d, off + 8), *struct.unpack_from('<IIII', d, off + 24)))
                for s in range(nsects):
                    so = off + 56 + s * 68
                    self.sections.append({'name': cstr(d, so), 'seg': cstr(d, so + 16),
                                          'addr': u32(d, so + 32), 'size': u32(d, so + 36)})
            elif base == LC_SYMTAB:
                self.symtab = struct.unpack_from('<IIII', d, off + 8)
            elif base in (LC_DYLD_INFO, LC_DYLD_INFO_ONLY):
                self.dyld = struct.unpack_from('<10I', d, off + 8)
            elif base == LC_MAIN:
                self.entry = struct.unpack_from('<Q', d, off + 8)[0]
            elif base == LC_UNIXTHREAD and u32(d, off + 8) in (1, 6):
                self.entry = u32(d, off + 8 + 8 + 15 * 4)
            off += cmdsize
        self.defined = {}
        self._read_symtab()
        self.text = next((s for s in self.segments if s[0] == '__TEXT'), None)
        if self.text and self.entry:
            self.entry = self.text[1] + (self.entry - self.text[3])
        self.slots = []
        self.rebases = 0
        if self.dyld:
            rebase_off, rebase_size, bind_off, bind_size, weak_off, weak_size, lazy_off, lazy_size, _, _ = self.dyld
            if weak_size:
                raise SystemExit('weak binds are not modelled here')
            self._binds(d[bind_off:bind_off + bind_size], 'bind')
            self._binds(d[lazy_off:lazy_off + lazy_size], 'lazy')
            self.rebases = self._rebases(d[rebase_off:rebase_off + rebase_size])

    def _read_symtab(self):
        if not self.symtab:
            return
        symoff, nsyms, stroff, strsize = self.symtab
        for i in range(nsyms):
            o = symoff + i * 12
            n_strx, n_type, n_value = u32(self.d, o), self.d[o + 4], u32(self.d, o + 8)
            if n_strx >= strsize or not n_value or (n_type & 0x0E) not in (0x0E, 0x02):
                continue
            self.defined.setdefault(cstr(self.d, stroff + n_strx), n_value)

    def _binds(self, stream, kind):
        seg = off = seg_off = ordinal = addend = 0
        sym = ''
        while off < len(stream):
            b = stream[off]
            off += 1
            op, imm = b & BIND_OPCODE_MASK, b & BIND_IMMEDIATE_MASK
            if op == BIND_OPCODE_DONE:
                if kind == 'bind':
                    break
                continue
            if op == BIND_OPCODE_SET_DYLIB_ORDINAL_IMM:
                ordinal = imm
            elif op == BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB:
                ordinal, off = uleb(stream, off)
            elif op == BIND_OPCODE_SET_DYLIB_SPECIAL_IMM:
                ordinal = imm - 16 if imm else 0
            elif op == BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM:
                sym, off = cstr(stream, off), stream.index(b'\0', off) + 1
            elif op == BIND_OPCODE_SET_TYPE_IMM:
                pass
            elif op == BIND_OPCODE_SET_ADDEND_SLEB:
                addend, off = sleb(stream, off)
            elif op == BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
                seg = imm
                seg_off, off = uleb(stream, off)
            elif op == BIND_OPCODE_ADD_ADDR_ULEB:
                delta, off = uleb(stream, off)
                seg_off = (seg_off + delta) & 0xFFFFFFFF
            elif op == BIND_OPCODE_DO_BIND:
                self._slot(seg, seg_off, sym, ordinal, addend, kind)
                seg_off += 4
            elif op == BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB:
                self._slot(seg, seg_off, sym, ordinal, addend, kind)
                delta, off = uleb(stream, off)
                seg_off += 4 + delta
            elif op == BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED:
                self._slot(seg, seg_off, sym, ordinal, addend, kind)
                seg_off += 4 + imm * 4
            elif op == BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB:
                count, off = uleb(stream, off)
                skip, off = uleb(stream, off)
                for _ in range(count):
                    self._slot(seg, seg_off, sym, ordinal, addend, kind)
                    seg_off += 4 + skip
            else:
                raise SystemExit(f'unknown bind opcode 0x{b:02x} at {off - 1}')

    def _slot(self, seg, seg_off, sym, ordinal, addend, kind):
        self.slots.append({'va': (self.segments[seg][1] + seg_off) & 0xFFFFFFFF, 'symbol': sym,
                           'ordinal': ordinal, 'addend': addend, 'kind': kind})

    def _rebases(self, stream):
        count = off = 0
        while off < len(stream):
            b = stream[off]
            off += 1
            op, imm = b & 0xF0, b & 0x0F
            if op == 0x00:
                break
            elif op == 0x10:
                pass
            elif op == 0x20:
                _, off = uleb(stream, off)
                count += 1
            elif op == 0x30:
                _, off = uleb(stream, off)
                _, off = uleb(stream, off)
                count += 1
            elif op == 0x40:
                count += 1
            elif op == 0x50:
                count += imm + 1
            elif op == 0x60:
                _, off = uleb(stream, off)
                count += 1
            elif op in (0x70, 0x80):
                count += 1
            else:
                break
        return count

    def section_at(self, va):
        for s in self.sections:
            if s['addr'] <= va < s['addr'] + s['size']:
                return s
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('image')
    ap.add_argument('--from', dest='start')
    ap.add_argument('--depth', type=int, default=4)
    args = ap.parse_args()

    img = Image(args.image)
    print('segments: ' + ', '.join(f'{n}@{vm:08x}+{vs:x}' for n, vm, vs, _, _ in img.segments))
    print(f"rebases: {img.rebases}, bind slots: {len(img.slots)}, defined symbols: {len(img.defined)}, "
          f"entry: {img.entry:08x}")

    for s in img.slots:
        sec = img.section_at(s['va'])
        s['section'] = sec['name'] if sec else '?'
        s['is_function'] = s['kind'] == 'lazy' or s['section'] in ('__la_symbol_ptr', '__stubs', '__auth_stub_helper')
    kinds = {}
    for s in img.slots:
        key = (s['section'], 'function' if s['is_function'] else 'data')
        kinds[key] = kinds.get(key, 0) + 1
    print('slots by section/kind:')
    for (sec, kind), n in sorted(kinds.items(), key=lambda kv: -kv[1]):
        print(f'  {sec:22s} {kind:8s} {n}')

    data = [s for s in img.slots if not s['is_function']]
    print(f'data imports: {len({s["symbol"] for s in data})} unique')
    for s in data[:25]:
        print(f"  {s['va']:08x} {s['section']:20s} {s['symbol']}")

    if not args.start:
        return 0
    if capstone is None:
        raise SystemExit('capstone is not installed: pip install capstone')

    start = int(args.start, 16) if args.start.startswith('0x') else img.defined.get(args.start)
    if start is None:
        raise SystemExit(f'{args.start}: not a defined symbol')

    slot_at = {s['va']: s for s in img.slots}
    md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)

    def name_at(va):
        for name, value in img.defined.items():
            if value == va:
                return name
        return ''

    def walk(va, depth, indent, seen):
        print(f'{indent}{va:08x} {name_at(va)}')
        if depth <= 0 or va in seen:
            return
        seen = seen | {va}
        literals, calls = {}, []
        for ins in md.disasm(img.d[va:va + 8192], va):
            m, ops = ins.mnemonic, ins.op_str
            if m in ('ldr', 'add') and ops.startswith('r') and '[pc' in ops:
                reg, rest = ops.split(',', 1)
                imm = int(rest.split('#')[1].split(']')[0], 0) if '#' in rest else 0
                literals[reg.strip()] = ins.address + 8 + imm
            if m in ('bl', 'blx') and ops.startswith('#'):
                calls.append(('direct', int(ops[1:], 0)))
            elif m == 'blx' and ops in literals:
                calls.append(('slot', literals[ops]))
            if (m == 'bx' and 'lr' in ops) or (m == 'pop' and 'pc' in ops):
                break
            if m == 'b' and ops.startswith('#') and int(ops[1:], 0) < va:
                break
        for kind, tgt in calls:
            if kind == 'slot':
                slot = slot_at.get(tgt)
                if slot:
                    print(f'{indent}  -> {slot["symbol"]} [{slot["section"]}, '
                          f'{"fn" if slot["is_function"] else "data"}]')
                else:
                    print(f'{indent}  -> slot {tgt:08x} (not bound)')
            elif any(s['addr'] <= tgt < s['addr'] + s['size'] for s in img.sections):
                walk(tgt, depth - 1, indent + '  ', seen)
            else:
                print(f'{indent}  -> extern {tgt:08x}')

    walk(start, args.depth, '', frozenset())
    return 0


if __name__ == '__main__':
    sys.exit(main())
