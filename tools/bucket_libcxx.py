import os
import subprocess
from pathlib import Path
from macho_runner import MachOBinary
from game_config import unity_framework_path

# llvm-cxxfilt comes from the NDK. Override with CXXFILT, or point ANDROID_SDK at your install.
_SDK = Path(os.environ.get("ANDROID_SDK") or Path(os.environ.get("LOCALAPPDATA", Path.home() / "AppData" / "Local")) / "Android" / "Sdk")
CXXFILT = os.environ.get("CXXFILT") or str(_SDK / "ndk/28.2.13676358/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-cxxfilt.exe")

b = MachOBinary(unity_framework_path())
libcxx_bindings = []
for bnd in b.bindings:
    if bnd.ordinal > 0 and bnd.ordinal <= len(b.dylibs):
        dylib = b.dylibs[bnd.ordinal - 1]
        if "libc++" in dylib:
            libcxx_bindings.append(bnd)

print(f"Total libc++ binding slots: {len(libcxx_bindings)}")
unique_symbols = sorted(list(set(bnd.symbol for bnd in libcxx_bindings)))
print(f"Unique libc++ symbols: {len(unique_symbols)}")

# Run llvm-cxxfilt on all unique symbols
# Note: Mach-O symbols have a leading underscore that cxxfilt might need stripped or llvm-cxxfilt handles directly
# Let's strip leading underscore for C++ mangled symbols starting with '__Z'
input_lines = []
for sym in unique_symbols:
    if sym.startswith("__Z"):
        input_lines.append(sym[1:])
    else:
        input_lines.append(sym)

proc = subprocess.run([CXXFILT], input="\n".join(input_lines), text=True, capture_output=True, check=True)
demangled = proc.stdout.splitlines()

sym_map = dict(zip(unique_symbols, demangled))

# Bucketing
buckets = {
    "vtable": [],
    "typeinfo": [],
    "operator new/delete": [],
    "__cxxabiv1 / exception ABI": [],
    "std::__1::basic_string": [],
    "std::__1 iostream / streambuf": [],
    "std::__1 locale / ctype": [],
    "std::__1 chrono / mutex / thread / cv": [],
    "std::__1 other": [],
    "other libc++": []
}

for sym, dem in sym_map.items():
    if sym.startswith("__ZTV"):
        buckets["vtable"].append((sym, dem))
    elif sym.startswith("__ZTI") or sym.startswith("__ZTS"):
        buckets["typeinfo"].append((sym, dem))
    elif "operator new" in dem or "operator delete" in dem:
        buckets["operator new/delete"].append((sym, dem))
    elif "__cxa" in dem or "__cxxabiv1" in dem or "type_info" in dem:
        buckets["__cxxabiv1 / exception ABI"].append((sym, dem))
    elif "basic_string" in dem:
        buckets["std::__1::basic_string"].append((sym, dem))
    elif any(x in dem for x in ["basic_ostream", "basic_istream", "basic_iostream", "streambuf", "basic_filebuf", "cin", "cout", "cerr"]):
        buckets["std::__1 iostream / streambuf"].append((sym, dem))
    elif any(x in dem for x in ["locale", "ctype", "numpunct", "num_get", "num_put", "codecvt"]):
        buckets["std::__1 locale / ctype"].append((sym, dem))
    elif any(x in dem for x in ["chrono", "mutex", "thread", "condition_variable", "lock"]):
        buckets["std::__1 chrono / mutex / thread / cv"].append((sym, dem))
    elif "std::__1" in dem or "std::" in dem:
        buckets["std::__1 other"].append((sym, dem))
    else:
        buckets["other libc++"].append((sym, dem))

print("\n=== LIBC++ IMPORT BUCKET SUMMARY ===")
for cat, lst in buckets.items():
    print(f"  {cat:36s}: {len(lst):4d} unique symbols")

print("\n=== VTABLE IMPORTS ===")
for sym, dem in buckets["vtable"]:
    print(f"  {sym:30s} -> {dem}")

print("\n=== TYPEINFO IMPORTS (Sample) ===")
for sym, dem in buckets["typeinfo"][:15]:
    print(f"  {sym:30s} -> {dem}")

print("\n=== OPERATOR NEW/DELETE IMPORTS ===")
for sym, dem in buckets["operator new/delete"]:
    print(f"  {sym:30s} -> {dem}")

print("\n=== __CXXABIV1 / EXCEPTION ABI ===")
for sym, dem in buckets["__cxxabiv1 / exception ABI"]:
    print(f"  {sym:30s} -> {dem}")
