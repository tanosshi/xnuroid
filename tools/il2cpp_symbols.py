#!/usr/bin/env python3
"""Join IL2CPP method pointer arrays with global-metadata.dat names, then resolve addresses.

The loader dumps the runtime half (LOADER_IL2CPP_MODULES), because only it can read guest memory:

    IMAGE <base> <size>
    MODULE <assembly.dll> <count> <pointers>
    M <assembly.dll> <index> <addr>

`index` is the position in that assembly's method pointer array, which IL2CPP fills in metadata token order.
tools/il2cpp_metadata.py lists methods in exactly that order, so index -> name is a direct lookup.

Usage:
    il2cpp_symbols.py join    <global-metadata.dat> <modules.txt> <out-syms.txt>
    il2cpp_symbols.py resolve <out-syms.txt> <addr> [addr ...]
    il2cpp_symbols.py top     <out-syms.txt> <count>

Syms file lines are "<addr-hex> <Namespace.Type::Method>"; `resolve` accepts hex addresses with or without 0x.
"""

import sys

import il2cpp_metadata


def read_modules(path):
    image = None
    modules = {}   # name -> {"count": int, "ptrs": int, "entries": [(index, addr)]}
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            parts = line.split()
            if not parts:
                continue
            if parts[0] == "IMAGE" and len(parts) >= 3:
                image = (int(parts[1], 16), int(parts[2], 16))
            elif parts[0] == "MODULE" and len(parts) >= 3:
                modules.setdefault(parts[1], {"count": int(parts[2]), "ptrs": int(parts[3], 16), "entries": []})
            elif parts[0] == "M" and len(parts) >= 4:
                modules.setdefault(parts[1], {"count": 0, "ptrs": 0, "entries": []})
                modules[parts[1]]["entries"].append((int(parts[2]), int(parts[3], 16)))
    return image, modules


def join(metadata_path, modules_path, out_path):
    md = il2cpp_metadata.load(metadata_path)
    by_name = {image["name"]: image for image in md.images()}
    image, modules = read_modules(modules_path)
    if image:
        print("image base 0x%x size 0x%x, %d modules" % (image[0], image[1], len(modules)))
    rows = []
    for name, module in sorted(modules.items()):
        meta_image = by_name.get(name)
        if not meta_image:
            print("  %-32s %6d pointers, no metadata image (skipped)" % (name, len(module["entries"])))
            continue
        names = md.image_methods(meta_image)
        count = len(module["entries"])
        good = min(count, len(names))
        if count != len(names):
            print("  %-32s %6d pointers vs %6d metadata methods (using %d)" % (name, count, len(names), good))
        else:
            print("  %-32s %6d methods" % (name, count))
        skipped = 0
        for index, addr in module["entries"]:
            if index >= len(names):
                continue
            # Burst-compiled method bodies live outside the image; a PC there would never be seen as image code.
            if image and not (image[0] <= addr < image[0] + image[1]):
                skipped += 1
                continue
            rows.append((addr, names[index][1]))
        if skipped:
            print("  %-32s %6d entries outside the image image (Burst)" % (name, skipped))
    rows.sort()
    # Store image offsets, not absolute addresses: the pointer tables are ASLR-relative and a syms file is reused by
    # later runs of the same build. The loader rebases them onto its own image base when it loads the file.
    base = image[0] if image else 0
    with open(out_path, "w", encoding="utf-8") as handle:
        handle.write("# image offsets, rebased by the loader\n")
        for addr, name in rows:
            handle.write("%x %s\n" % (addr - base, name))
    print("wrote %d symbols -> %s" % (len(rows), out_path))
    return rows


def load_syms(path):
    """Read a syms file; values stay image offsets, so pass --base to resolve absolute addresses."""
    rows = []
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            if line.startswith("#"):
                continue
            parts = line.split(" ", 1)
            if len(parts) != 2:
                continue
            rows.append((int(parts[0], 16), parts[1].strip()))
    rows.sort()
    return rows


def lookup(rows, addr):
    """Innermost method containing addr, bounded by the next method entry."""
    lo, hi = 0, len(rows)
    while lo < hi:
        mid = (lo + hi) // 2
        if rows[mid][0] <= addr:
            lo = mid + 1
        else:
            hi = mid
    if lo == 0:
        return None, 0
    start, name = rows[lo - 1]
    if lo < len(rows) and rows[lo][0] == addr:
        return None, 0
    return name, addr - start


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    command = argv[1]
    if command == "join":
        if len(argv) < 5:
            print("join needs <global-metadata.dat> <modules.txt> <out-syms.txt>")
            return 2
        join(argv[2], argv[3], argv[4])
        return 0
    if command == "resolve":
        rows = load_syms(argv[2])
        base = 0
        args = argv[3:]
        if args and args[0] == "--base" and len(args) > 1:
            base = int(args[1], 16)
            args = args[2:]
        for text in args:
            addr = int(text, 16)
            if base and addr >= base:
                addr -= base        # accept absolute addresses, look up the offset
            name, offset = lookup(rows, addr)
            print("offset 0x%x %s" % (addr, ("%s+0x%x" % (name, offset)) if name else "<unmanaged or between methods>"))
        return 0
    if command == "top":
        rows = load_syms(argv[2])
        limit = int(argv[3]) if len(argv) > 3 else 20
        print("%d symbols, %d bytes of names" % (len(rows), sum(len(n) for _, n in rows)))
        return 0
    print("unknown command %r" % command)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
