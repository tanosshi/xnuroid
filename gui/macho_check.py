"""Tells whether the Mach-O binaries of an iOS app are still FairPlay encrypted.

Every App Store binary carries an LC_ENCRYPTION_INFO(_64) load command. Its cryptid field is 1 while the
__TEXT pages are encrypted and 0 once a dumper has written the decrypted pages back.
"""
from __future__ import annotations

import plistlib
import struct
import zipfile
from dataclasses import dataclass, field
from pathlib import Path, PurePosixPath
from typing import BinaryIO, Iterable

MH_MAGIC = 0xFEEDFACE
MH_MAGIC_64 = 0xFEEDFACF
FAT_MAGIC = 0xCAFEBABE
FAT_MAGIC_64 = 0xCAFEBABF
LC_ENCRYPTION_INFO = 0x21
LC_ENCRYPTION_INFO_64 = 0x2C
CPU_TYPE_ARM64 = 0x0100000C
MAX_FAT_ARCHES = 32


@dataclass
class BinaryReport:
    name: str
    encrypted: bool | None = None
    arm64: bool = False
    problem: str = ""


@dataclass
class AppReport:
    app_name: str = ""
    binaries: list[BinaryReport] = field(default_factory=list)

    @property
    def main(self) -> BinaryReport | None:
        return self.binaries[0] if self.binaries else None

    @property
    def encrypted(self) -> list[BinaryReport]:
        return [b for b in self.binaries if b.encrypted]

    @property
    def unreadable(self) -> list[BinaryReport]:
        return [b for b in self.binaries if b.encrypted is None]

    @property
    def usable(self) -> bool:
        main = self.main
        return not self.encrypted and main is not None and main.encrypted is not None and main.arm64


def _read_exact(stream: BinaryIO, size: int) -> bytes:
    data = stream.read(size)
    if len(data) != size:
        raise ValueError("file ends early")
    return data


def _thin_slice(stream: BinaryIO, offset: int, report: BinaryReport) -> None:
    stream.seek(offset)
    header = _read_exact(stream, 28)
    magic_le = struct.unpack_from("<I", header)[0]
    if magic_le not in (MH_MAGIC, MH_MAGIC_64):
        raise ValueError("not a Mach-O slice")
    is_64 = magic_le == MH_MAGIC_64
    cputype, _, _, ncmds, sizeofcmds = struct.unpack_from("<iIIII", header, 4)
    report.arm64 = cputype == CPU_TYPE_ARM64
    if is_64:
        _read_exact(stream, 4)
    commands = _read_exact(stream, sizeofcmds)
    cursor = 0
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from("<II", commands, cursor)
        if cmd in (LC_ENCRYPTION_INFO, LC_ENCRYPTION_INFO_64):
            report.encrypted = struct.unpack_from("<I", commands, cursor + 16)[0] != 0
            return
        if cmdsize < 8:
            raise ValueError("broken load command")
        cursor += cmdsize
    report.encrypted = False


def inspect_stream(stream: BinaryIO, name: str) -> BinaryReport:
    report = BinaryReport(name)
    try:
        head = _read_exact(stream, 8)
        magic_be = struct.unpack_from(">I", head)[0]
        if magic_be in (FAT_MAGIC, FAT_MAGIC_64):
            count = struct.unpack_from(">I", head, 4)[0]
            if not 0 < count <= MAX_FAT_ARCHES:
                raise ValueError("not a Mach-O file")
            entry = 32 if magic_be == FAT_MAGIC_64 else 20
            table = _read_exact(stream, entry * count)
            slices = []
            for index in range(count):
                if magic_be == FAT_MAGIC_64:
                    cputype, _, offset = struct.unpack_from(">iIQ", table, index * entry)
                else:
                    cputype, _, offset = struct.unpack_from(">iII", table, index * entry)
                slices.append((cputype, offset))
            arm64 = [offset for cputype, offset in slices if cputype == CPU_TYPE_ARM64]
            _thin_slice(stream, arm64[0] if arm64 else slices[0][1], report)
        else:
            _thin_slice(stream, 0, report)
    except (ValueError, struct.error, OSError, zipfile.BadZipFile) as error:
        report.encrypted = None
        report.problem = str(error)
    return report


def _framework_binaries(names: Iterable[PurePosixPath], app: PurePosixPath) -> list[PurePosixPath]:
    frameworks = app / "Frameworks"
    dylibs = [p for p in names if p.parent == frameworks and p.suffix == ".dylib"]
    bundled = [p for p in names if p.parent.parent == frameworks and p.parent.suffix == ".framework"
               and p.name == p.parent.stem]
    return sorted(dylibs) + sorted(bundled)


def check_ipa(ipa: Path) -> AppReport:
    with zipfile.ZipFile(ipa) as archive:
        members = {PurePosixPath(n): n for n in archive.namelist() if not n.endswith("/")}
        apps = sorted({PurePosixPath(*p.parts[:2]) for p in members
                       if len(p.parts) >= 3 and p.parts[0] == "Payload" and p.parts[1].endswith(".app")})
        if not apps:
            raise ValueError("there is no Payload/<App>.app inside this file")
        app = apps[0]
        plist = members.get(app / "Info.plist")
        info = plistlib.loads(archive.read(plist)) if plist else {}
        executable = app / (info.get("CFBundleExecutable") or app.stem)
        report = AppReport(app.name)
        for path in [executable] + _framework_binaries(members, app):
            member = members.get(path)
            if member is None:
                report.binaries.append(BinaryReport(path.name, None, problem="missing from the app"))
                continue
            with archive.open(member) as stream:
                report.binaries.append(inspect_stream(stream, path.name))
        return report


def check_app(app: Path) -> AppReport:
    try:
        with (app / "Info.plist").open("rb") as handle:
            info = plistlib.load(handle)
    except (OSError, plistlib.InvalidFileException):
        info = {}
    executable = app / (info.get("CFBundleExecutable") or app.stem)
    frameworks = app / "Frameworks"
    extra = []
    if frameworks.is_dir():
        extra += sorted(frameworks.glob("*.dylib"))
        extra += sorted(fw / fw.stem for fw in frameworks.glob("*.framework") if (fw / fw.stem).is_file())
    report = AppReport(app.name)
    for path in [executable] + extra:
        if not path.is_file():
            report.binaries.append(BinaryReport(path.name, None, problem="missing from the app"))
            continue
        with path.open("rb") as stream:
            report.binaries.append(inspect_stream(stream, path.name))
    return report
