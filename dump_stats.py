import json
from collections import Counter
from pathlib import Path

inv = json.load(open('inventory.json'))
print('=== ARCHITECTURE & CPU SUBTYPE ===')
for m in inv['macho_files']:
    sl = m['slices'][0]
    arch = sl['architecture']
    cputype = arch['cputype']
    subtype = arch['cpusubtype_raw']
    # Check capability bits: CPU_SUBTYPE_ARM64_ALL is 0. CPU_SUBTYPE_ARM64_V8 is 1. CPU_SUBTYPE_ARM64E is 2.
    print(f"{m['path']}: cputype=0x{cputype:x} (CPU_TYPE_ARM64), cpusubtype=0x{subtype:x} (CPU_SUBTYPE_ARM64_ALL)")

print('\n=== IMPORT HISTOGRAM PER DYLIB (UnityFramework) ===')
unity = inv['macho_files'][1]['slices'][0]
bindings = unity['dyld_info']['bindings']

dylibs = Counter()
dylib_strong = Counter()
dylib_weak = Counter()
objc_classes = set()

for b in bindings:
    lib = b['library']
    dylibs[lib] += 1
    if b['weak']:
        dylib_weak[lib] += 1
    else:
        dylib_strong[lib] += 1
    
    sym = b['symbol']
    if 'OBJC_CLASS_$_' in sym:
        objc_classes.add(sym.replace('_OBJC_CLASS_$_', ''))

for lib, total in dylibs.most_common():
    s = dylib_strong[lib]
    w = dylib_weak[lib]
    print(f"  {lib:65s}: {total:4d} (strong: {s:4d}, weak: {w:4d})")

print(f'\n=== ALL {len(objc_classes)} DISTINCT EXTERNAL OBJECTIVE-C CLASSES ===')
for i, c in enumerate(sorted(list(objc_classes)), 1):
    print(f"  {i:2d}. {c}")
