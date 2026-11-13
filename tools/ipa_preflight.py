#!/usr/bin/env python3
"""Answer "would this bundle boot in the loader?" from the binary alone, with no device.

Usage: python tools/ipa_preflight.py <bundle.ipa|bundle.app> [--json]

Reads Info.plist and the Mach-O executable, classifies every imported symbol against the provider
tables in src/app and src/loader, and applies the same rule the loader's load-time preflight uses:
an app-mode bundle that imports Swift ABI symbols cannot boot (there is no Swift runtime).

Exit status: 0 bootable, 3 Swift runtime required, 4 encrypted/undecryptable, 2 usage or parse error.
"""
from __future__ import annotations

import glob
import json
import os
import plistlib
import re
import struct
import sys
import zipfile

MH_MAGIC_64 = 0xFEEDFACF
FAT_MAGIC = 0xCAFEBABE
CPU_ARM64 = 0x0100000C
LC_SEGMENT_64 = 0x19
LC_DYLD_INFO = (0x22, 0x80000022)
LC_SYMTAB = 0x2
LC_DYLIB = (0xC, 0x80000018, 0x8000001F, 0x80000023)  # LOAD, LOAD_WEAK, REEXPORT, LOAD_UPWARD
LC_MAIN = 0x80000028
LC_ENCRYPTION_INFO_64 = 0x2C
LC_BUILD_VERSION = 0x32
LC_CHAINED_FIXUPS = 0x80000034
LC_VERSION_MIN = (0x24, 0x25, 0x2F, 0x30)


def read_bundle(path):
    """Return (info_plist_dict, executable_bytes, display_name) for a .ipa or .app."""
    if os.path.isdir(path):
        pl = plistlib.load(open(os.path.join(path, "Info.plist"), "rb"))
        exe = os.path.join(path, pl["CFBundleExecutable"])
        return pl, open(exe, "rb").read(), os.path.basename(path), os.path.isdir(os.path.join(path, "SC_Info"))
    z = zipfile.ZipFile(path)
    apps = sorted({n.split("/")[1] for n in z.namelist() if n.startswith("Payload/") and n.count("/") > 1})
    if len(apps) != 1:
        raise SystemExit("[preflight] expected exactly one Payload/*.app, found %r" % apps)
    app = "Payload/" + apps[0]
    pl = plistlib.loads(z.read(app + "/Info.plist"))
    sc_info = any(n.startswith(app + "/SC_Info/") for n in z.namelist())
    return pl, z.read(app + "/" + pl["CFBundleExecutable"]), apps[0], sc_info


def thin_slice(data):
    """Pick the arm64 slice of a thin or fat Mach-O."""
    magic = struct.unpack_from(">I", data, 0)[0]
    if magic == FAT_MAGIC:
        n = struct.unpack_from(">I", data, 4)[0]
        for i in range(n):
            cpu, _sub, off, size, _align = struct.unpack_from(">IIIII", data, 8 + 20 * i)
            if cpu == CPU_ARM64:
                return data[off:off + size]
        raise SystemExit("[preflight] fat binary without an arm64 slice")
    return data


def uleb(d, p):
    r = s = 0
    while True:
        b = d[p]
        p += 1
        r |= (b & 0x7F) << s
        s += 7
        if not b & 0x80:
            return r, p


def sleb(d, p):
    r = s = 0
    while True:
        b = d[p]
        p += 1
        r |= (b & 0x7F) << s
        s += 7
        if not b & 0x80:
            if b & 0x40:
                r -= 1 << s
            return r, p


def parse_macho(d):
    if struct.unpack_from("<I", d, 0)[0] != MH_MAGIC_64:
        raise SystemExit("[preflight] not a 64-bit little-endian Mach-O")
    cpu, sub = struct.unpack_from("<II", d, 4)
    ncmds = struct.unpack_from("<I", d, 16)[0]
    out = {"cputype": cpu, "cpusubtype": sub, "ncmds": ncmds, "segs": [], "dylibs": [],
           "chained": None, "entry": None, "cryptid": None, "minos": None, "sdk": None,
           "dyld_info": None, "imports": []}
    off = 32
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", d, off)
        if cmd == LC_SEGMENT_64:
            name = d[off + 8:off + 24].split(b"\0")[0].decode("ascii", "replace")
            vm, vs, fo, fs = struct.unpack_from("<QQQQ", d, off + 24)
            out["segs"].append((name, vm, vs, fo, fs))
        elif cmd in LC_DYLIB:
            so = struct.unpack_from("<I", d, off + 8)[0]
            out["dylibs"].append(d[off + so:off + size].split(b"\0")[0].decode("ascii", "replace"))
        elif cmd == LC_MAIN:
            out["entry"] = struct.unpack_from("<Q", d, off + 8)[0]
        elif cmd == LC_ENCRYPTION_INFO_64:
            out["cryptid"] = struct.unpack_from("<III", d, off + 8)[2]
        elif cmd == LC_BUILD_VERSION:
            plat, minos, sdk = struct.unpack_from("<III", d, off + 8)
            out["minos"] = "%d.%d.%d" % (minos >> 16, (minos >> 8) & 0xFF, minos & 0xFF)
            out["sdk"] = "%d.%d.%d" % (sdk >> 16, (sdk >> 8) & 0xFF, sdk & 0xFF)
        elif cmd in LC_VERSION_MIN:
            v = struct.unpack_from("<I", d, off + 8)[0]
            out["minos"] = "%d.%d.%d" % (v >> 16, (v >> 8) & 0xFF, v & 0xFF)
            out["sdk"] = out["sdk"] or None
        elif cmd == LC_CHAINED_FIXUPS:
            out["chained"] = struct.unpack_from("<II", d, off + 8)
        elif cmd in LC_DYLD_INFO:
            out["dyld_info"] = struct.unpack_from("<II", d, off + 8)[0:2] + \
                               struct.unpack_from("<II", d, off + 32)[0:2]
        off += size
    return out


def chained_imports(d, chained):
    off, size = chained
    cd = d[off:off + size]
    _ver, _starts, imp_off, sym_off, count, fmt, _sfmt = struct.unpack_from("<IIIIIII", cd, 0)
    if fmt not in (1, 2, 3):
        raise SystemExit("[preflight] unknown chained import format %d" % fmt)
    stride = {1: 4, 2: 8, 3: 16}[fmt]
    names = []
    for i in range(count):
        at = imp_off + i * stride
        if fmt == 1:
            word = struct.unpack_from("<I", cd, at)[0]
            lib, name_off = word & 0xFF, word >> 9
        elif fmt == 2:
            lib = cd[at]
            name_off = struct.unpack_from("<I", cd, at + 4)[0]
        else:
            lib = struct.unpack_from("<H", cd, at)[0]
            name_off = struct.unpack_from("<I", cd, at + 4)[0]
        end = cd.index(b"\0", sym_off + name_off)
        names.append((cd[sym_off + name_off:end].decode("utf-8", "replace"), lib, False))
    return names


def classic_imports(d, dyld_info):
    """Walk the bind and lazy-bind streams; symbol names are what matter here."""
    bind_off, bind_size, lazy_off, lazy_size = dyld_info
    names = []
    for is_lazy, (off, size) in ((False, (bind_off, bind_size)), (True, (lazy_off, lazy_size))):
        if not size:
            continue
        p, end = off, off + size
        ordinal = 0
        while p < end:
            op, imm = d[p] & 0xF0, d[p] & 0x0F
            p += 1
            if op == 0x00:
                continue
            if op == 0x10:
                ordinal = imm
            elif op == 0x20:
                ordinal, p = uleb(d, p)
            elif op == 0x30:
                ordinal = imm | 0xF0 if imm else 0
            elif op == 0x40:
                end_sym = d.index(b"\0", p)
                names.append((d[p:end_sym].decode("utf-8", "replace"), ordinal, is_lazy))
                p = end_sym + 1
            elif op == 0x50:
                pass
            elif op == 0x60:
                _, p = sleb(d, p)
            elif op in (0x70, 0x80):
                _, p = uleb(d, p)
            elif op == 0xA0:
                _, p = uleb(d, p)
            elif op == 0xC0:
                _, p = uleb(d, p)
                _, p = uleb(d, p)
            elif op in (0x90, 0xB0):
                pass
            else:
                break
    return names


def providers(repo):
    """Every guest symbol the app tables answer, including the m[std::string("_") + n] form."""
    names = set()
    files = glob.glob(os.path.join(repo, "src/app/*.cpp")) + [os.path.join(repo, "src/loader/shims.cpp"),
                                                              os.path.join(repo, "src/loader/android_loader.cpp")]
    for f in files:
        with open(f, encoding="utf-8", errors="ignore") as fh:
            for lit in re.findall(r'"([A-Za-z_][A-Za-z0-9_$.]*)"', fh.read()):
                names.add(lit)
                names.add("_" + lit)
    # register_runtime_symbols installs these aliases in a loop, so they have no literal of their own.
    for i in range(1, 29):
        names.add("_objc_retain_x%d" % i)
        names.add("_objc_release_x%d" % i)
    return names


FUNCTION_HEADS = ("_objc_opt_self", "_NSDecimalRound", "_CTFontManager",
                  "_objc_retain_x", "_objc_release_x", "_swift_", "___objc_personality_v0",
                  "___gxx_personality_v0", "___chkstk_darwin")


def is_function_name(name):
    if name.startswith(FUNCTION_HEADS):
        return True
    return name.startswith("_objc_")  # single-underscore objc entries are all C functions


def is_data_constant(name):
    """Mirrors app::data_constant: single leading underscore, alphanumerics and '_' only."""
    return bool(re.fullmatch(r"_[A-Za-z0-9_]{3,}", name)) and not name.startswith("__")


LIBC_OWNERS = ("libSystem.B.dylib", "libc++.1.dylib", "libobjc.A.dylib")


def owner_of(lib, libs):
    if lib == 0:
        return "main-executable"
    if lib >= 0xFD:
        return "flat/weak lookup"
    return libs[lib - 1].split("/")[-1] if 0 < lib <= len(libs) else "ord%d" % lib


def classify(imports, prov, libs=()):
    is_class = lambda s: s.startswith("_OBJC_CLASS_$_") or s.startswith("_OBJC_METACLASS_$_")
    is_swift = lambda s: s.startswith("_$s") or s.startswith("_swift_")
    rows = {"provided": [], "class": [], "constant": [], "libc": [], "swift": [], "other": []}
    for name, lib, lazy in imports:
        if name in prov:
            rows["provided"].append(name)
        elif is_class(name):
            rows["class"].append(name)
        elif is_swift(name):
            rows["swift"].append(name)
        elif owner_of(lib, libs) in LIBC_OWNERS or owner_of(lib, libs) == "flat/weak lookup":
            rows["libc"].append(name)      # resolved through dlsym on bionic before any fallback
        elif not lazy and is_data_constant(name) and not is_function_name(name):
            rows["constant"].append(name)
        else:
            rows["other"].append(name)
    return rows


def main(argv):
    as_json = "--json" in argv
    args = [a for a in argv[1:] if not a.startswith("--")]
    if len(args) != 1:
        print(__doc__.strip().splitlines()[2], file=sys.stderr)
        return 2
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    pl, raw, display, sc_info = read_bundle(args[0])
    d = thin_slice(raw)
    m = parse_macho(d)
    if m["chained"]:
        imports = chained_imports(d, m["chained"])
        stream = "chained fixups"
    elif m["dyld_info"]:
        imports = classic_imports(d, m["dyld_info"])
        stream = "classic dyld info"
    else:
        imports = []
        stream = "none"
    prov = providers(repo)
    rows = classify(imports, prov, m["dylibs"])
    total = len(imports)
    supported = len(rows["provided"]) + len(rows["class"]) + len(rows["constant"]) + len(rows["libc"])
    sw_abi = sum(1 for s in rows["swift"] if s.startswith("_$s"))
    sw_rt = len(rows["swift"]) - sw_abi
    app_mode = m["entry"] is not None and any(s == "_UIApplicationMain" for s, _l, _z in imports)
    if sc_info or (m["cryptid"] or 0) != 0:
        why = "SC_Info present, cryptid %s" % m["cryptid"] if sc_info else "cryptid %d" % m["cryptid"]
        verdict, code = "encrypted (%s): the code is still ciphertext" % why, 4
    elif m["entry"] is not None and rows["swift"]:
        verdict, code = "refuse: Swift runtime required (%d mangled _$s + %d _swift_ entries)" % (sw_abi, sw_rt), 3
    elif not imports:
        verdict, code = "unknown: no import stream found", 2
    else:
        verdict, code = "pass: no Swift ABI imports (other gaps can still stop it)", 0
    pct = 100.0 * supported / total if total else 0.0
    report = {
        "bundle": display, "minimum_os": m["minos"], "sdk": m["sdk"],
        "cputype": m["cputype"], "cryptid": m["cryptid"], "entry": m["entry"], "sc_info": sc_info,
        "stream": stream, "app_mode": app_mode, "imports": total,
        "provided": len(rows["provided"]), "objc_classes": len(rows["class"]), "constants": len(rows["constant"]),
        "libc": len(rows["libc"]),
        "swift": len(rows["swift"]), "swift_mangled": sw_abi, "swift_runtime": sw_rt,
        "other": len(rows["other"]), "supported": supported, "supported_pct": round(pct, 1),
        "verdict": verdict, "exit": code,
    }
    if as_json:
        print(json.dumps(report, indent=2))
    else:
        print("bundle      %s" % display)
        print("target      arm64 (cputype 0x%x) | minos %s | sdk %s | cryptid %s"
              % (m["cputype"], m["minos"], m["sdk"], m["cryptid"]))
        print("imports     %d via %s | app mode: %s" % (total, stream, "yes" if app_mode else "no"))
        print("supported   %d (%.1f%%): %d provided, %d ObjC classes stubbable, %d constants synthesized, %d via bionic"
              % (supported, pct, len(rows["provided"]), len(rows["class"]), len(rows["constant"]), len(rows["libc"])))
        print("missing     %d Swift (%d _$s, %d _swift_), %d other"
              % (len(rows["swift"]), sw_abi, sw_rt, len(rows["other"])))
        print("verdict     %s" % verdict)
    return code


if __name__ == "__main__":
    sys.exit(main(sys.argv))
