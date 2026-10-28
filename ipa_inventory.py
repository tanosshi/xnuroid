#!/usr/bin/env python3
"""Package-level IPA inventory wrapper for the C++ Mach-O inspector."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import plistlib
import struct
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

MACHO_MAGICS = {
    b"\xcf\xfa\xed\xfe", b"\xfe\xed\xfa\xcf",  # 64-bit little/big endian
    b"\xce\xfa\xed\xfe", b"\xfe\xed\xfa\xce",  # 32-bit little/big endian
    b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca",  # fat32
    b"\xca\xfe\xba\xbf", b"\xbf\xba\xfe\xca",  # fat64
}
FAT = {
    b"\xca\xfe\xba\xbe": (">", False), b"\xbe\xba\xfe\xca": ("<", False),
    b"\xca\xfe\xba\xbf": (">", True), b"\xbf\xba\xfe\xca": ("<", True),
}


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def load_plist(path: Path):
    try:
        with path.open("rb") as f:
            return plistlib.load(f)
    except Exception as exc:  # malformed or non-plist metadata is reported, not fatal
        return {"_parse_error": str(exc)}


def select_package_root(source: Path) -> Path | None:
    if source.is_dir() and source.suffix.lower() == ".app":
        return source
    if source.is_dir():
        apps = sorted(source.glob("Payload/*.app"))
        if apps:
            return apps[0]
        if (source / "Info.plist").is_file():
            return source
    return None


def read_candidate(path: Path):
    try:
        with path.open("rb") as f:
            magic = f.read(4)
        return magic if magic in MACHO_MAGICS else None
    except OSError:
        return None


def fat_slices(data: bytes):
    magic = data[:4]
    if magic not in FAT:
        return None
    endian, is64 = FAT[magic]
    if len(data) < 8:
        raise ValueError("truncated fat header")
    count = struct.unpack_from(endian + "I", data, 4)[0]
    entry_size = 32 if is64 else 20
    if count > (len(data) - 8) // entry_size:
        raise ValueError("fat architecture table exceeds file")
    result = []
    for i in range(count):
        pos = 8 + i * entry_size
        if is64:
            cpu, subtype, offset, size, align, _reserved = struct.unpack_from(endian + "IIQQII", data, pos)
        else:
            cpu, subtype, offset, size, align = struct.unpack_from(endian + "IIIII", data, pos)
        if offset > len(data) or size > len(data) - offset:
            raise ValueError(f"fat slice {i} exceeds file bounds")
        result.append({"index": i, "cputype": cpu, "cpusubtype_raw": subtype,
                       "offset": offset, "size": size, "align_exponent": align,
                       "bytes": data[offset:offset + size]})
    return result


def inspect_slice(inspector: Path, data: bytes, display_path: str, tempdir: Path):
    fd, temp_name = tempfile.mkstemp(prefix="macho-slice-", suffix=".bin", dir=tempdir)
    os.close(fd)
    temp_path = Path(temp_name)
    try:
        temp_path.write_bytes(data)
        proc = subprocess.run([str(inspector), str(temp_path)], capture_output=True, text=True)
        if proc.stdout.strip():
            try:
                report = json.loads(proc.stdout)
            except json.JSONDecodeError:
                report = {"error": "inspector returned invalid JSON", "stdout": proc.stdout[:4000]}
        else:
            report = {"error": proc.stderr.strip() or f"inspector exit {proc.returncode}"}
        report["source"] = display_path
        report["inspector_exit_code"] = proc.returncode
        return report
    finally:
        temp_path.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description="Inventory Mach-O binaries and package metadata in an IPA or extracted .app")
    parser.add_argument("source", type=Path, help=".ipa, extracted Payload directory, or .app directory")
    parser.add_argument("--inspector", type=Path, help="path to macho-inspect executable (default: sibling executable)")
    parser.add_argument("--output", type=Path, help="write JSON report here (default: stdout)")
    args = parser.parse_args()
    source = args.source.resolve()
    inspector = (args.inspector or (Path(__file__).with_name("macho-inspect.exe") if os.name == "nt" else Path(__file__).with_name("macho-inspect"))).resolve()
    if not inspector.is_file():
        parser.error(f"Mach-O inspector executable not found: {inspector}")

    version_proc = subprocess.run([str(inspector), "--version"], capture_output=True, text=True)
    tool_version = version_proc.stdout.strip() or "unknown"
    package_root = select_package_root(source)
    report = {"schema": "ipa-package-inventory-v1", "source": str(source),
              "inspector_version": tool_version, "macho_files": [], "package": {}, "warnings": []}

    with tempfile.TemporaryDirectory(prefix="ipa-inventory-") as td:
        tempdir = Path(td)
        candidates = []
        if source.is_file() and source.suffix.lower() == ".ipa":
            try:
                archive = zipfile.ZipFile(source)
                for info in archive.infolist():
                    if info.is_dir():
                        continue
                    try:
                        with archive.open(info) as stream:
                            magic = stream.read(4)
                    except Exception:
                        continue
                    if magic in MACHO_MAGICS:
                        # Stream only Mach-O members to a temporary file; resources stay in the archive.
                        target = tempdir / ("member-" + str(len(candidates)))
                        with archive.open(info) as src, target.open("wb") as dst:
                            while True:
                                block = src.read(1024 * 1024)
                                if not block: break
                                dst.write(block)
                        candidates.append((info.filename, target, sha256_file(target)))
                app_infos = [i for i in archive.infolist() if i.filename.endswith(".app/Info.plist") and "/Payload/" in i.filename]
                if app_infos:
                    with archive.open(app_infos[0]) as stream:
                        plist_data = plistlib.loads(stream.read())
                    report["package"]["info_plist"] = {k: plist_data.get(k) for k in (
                        "CFBundleIdentifier", "CFBundleName", "CFBundleExecutable", "CFBundleShortVersionString",
                        "MinimumOSVersion", "UIRequiredDeviceCapabilities", "UISupportedInterfaceOrientations",
                        "UIBackgroundModes", "DTPlatformName", "DTSDKName") if k in plist_data}
                report["package"]["mobileprovision_present"] = any(i.filename.endswith("embedded.mobileprovision") for i in archive.infolist())
                report["package"]["metallib_files"] = [i.filename for i in archive.infolist() if i.filename.endswith(".metallib")]
            except (OSError, zipfile.BadZipFile) as exc:
                parser.error(f"cannot read IPA archive: {exc}")
        elif package_root:
            plist_path = package_root / "Info.plist"
            plist_data = load_plist(plist_path) if plist_path.is_file() else {}
            report["package"]["info_plist"] = {k: plist_data.get(k) for k in (
                "CFBundleIdentifier", "CFBundleName", "CFBundleExecutable", "CFBundleShortVersionString",
                "MinimumOSVersion", "UIRequiredDeviceCapabilities", "UISupportedInterfaceOrientations",
                "UIBackgroundModes", "DTPlatformName", "DTSDKName") if k in plist_data}
            report["package"]["mobileprovision_present"] = (package_root / "embedded.mobileprovision").exists()
            report["package"]["metallib_files"] = [str(p.relative_to(package_root)) for p in package_root.rglob("*.metallib")]
            metadata = list(package_root.rglob("global-metadata.dat"))
            if metadata:
                with metadata[0].open("rb") as f: header = f.read(8)
                if len(header) >= 8:
                    report["package"]["il2cpp_metadata"] = {"path": str(metadata[0].relative_to(package_root)),
                        "magic_hex": header[:4].hex(), "version": int.from_bytes(header[4:8], "little"),
                        "expected_magic": int.from_bytes(header[:4], "little") == 0xFAB11BAF}
            for path in package_root.rglob("*"):
                if path.is_file() and read_candidate(path):
                    candidates.append((str(path.relative_to(package_root)), path, sha256_file(path)))
        else:
            parser.error("source must be an .ipa, a Payload directory, or an .app directory")

        report["package"].setdefault("metallib_files", [])
        for display, path, digest in candidates:
            data = path.read_bytes()
            entry = {"path": display, "sha256": digest, "size": len(data)}
            try:
                slices = fat_slices(data)
                if slices is None:
                    entry["slices"] = [inspect_slice(inspector, data, display, tempdir)]
                else:
                    entry["container"] = "fat64" if data[:4] in (b"\xca\xfe\xba\xbf", b"\xbf\xba\xfe\xca") else "fat32"
                    entry["slices"] = []
                    for sl in slices:
                        sub = inspect_slice(inspector, sl.pop("bytes"), display + f"#slice{sl['index']}", tempdir)
                        sl["inventory"] = sub
                        entry["slices"].append(sl)
            except ValueError as exc:
                entry["error"] = str(exc)
            report["macho_files"].append(entry)

    rendered = json.dumps(report, indent=2, sort_keys=True)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(rendered + "\n", encoding="utf-8")
    else:
        print(rendered)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
