"""Dump exported symbols (name -> image offset) from a thin arm64 Mach-O via its export trie.

Usage: python il2cpp_exports.py <UnityFramework> [prefix]
"""
import struct
import sys


def uleb(buf, pos):
    result = shift = 0
    while True:
        b = buf[pos]
        pos += 1
        result |= (b & 0x7F) << shift
        if not b & 0x80:
            return result, pos
        shift += 7


def walk(trie, pos, prefix, out):
    size, pos = uleb(trie, pos)
    if size:
        flags, p = uleb(trie, pos)
        if flags & 0x8:  # re-export: dylib ordinal + optional name
            _, p = uleb(trie, p)
        else:
            off, p = uleb(trie, p)
            if flags & 0x10:  # stub and resolver
                _, p = uleb(trie, p)
            out[prefix] = off
    pos += size
    nchild = trie[pos]
    pos += 1
    for _ in range(nchild):
        end = trie.index(b"\0", pos)
        label = trie[pos:end].decode()
        pos = end + 1
        child, pos = uleb(trie, pos)
        walk(trie, child, prefix + label, out)


def main():
    path = sys.argv[1]
    prefix = sys.argv[2] if len(sys.argv) > 2 else "_il2cpp_"
    data = open(path, "rb").read()
    magic, _, _, _, ncmds, _, _, _ = struct.unpack_from("<IiiIIIII", data, 0)
    assert magic == 0xFEEDFACF, hex(magic)
    pos = 32
    export_off = export_size = 0
    text_vmaddr = None
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from("<II", data, pos)
        if cmd == 0x19 and data[pos + 8:pos + 24].rstrip(b"\0") == b"__TEXT":
            text_vmaddr = struct.unpack_from("<Q", data, pos + 24)[0]
        if cmd in (0x22, 0x80000022):
            export_off, export_size = struct.unpack_from("<II", data, pos + 40)
        if cmd == 0x80000033:  # LC_DYLD_EXPORTS_TRIE
            export_off, export_size = struct.unpack_from("<II", data, pos + 8)
        pos += cmdsize
    trie = data[export_off:export_off + export_size]
    out = {}
    walk(trie, 0, "", out)
    for name, off in sorted(out.items(), key=lambda kv: kv[1]):
        if name.startswith(prefix):
            # export offsets are relative to __TEXT vmaddr (image base)
            print(f"{name} 0x{off:x}")


if __name__ == "__main__":
    main()
