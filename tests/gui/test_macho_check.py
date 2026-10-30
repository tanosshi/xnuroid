import io
import plistlib
import struct
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "gui"))
import macho_check  # noqa: E402

CPU_X86_64 = 0x01000007


def thin(cryptid: int | None, cputype: int = macho_check.CPU_TYPE_ARM64) -> bytes:
    commands = struct.pack("<II", 0x19, 72) + bytes(64)
    if cryptid is not None:
        commands += struct.pack("<IIIIII", macho_check.LC_ENCRYPTION_INFO_64, 24, 0x4000, 0x1000, cryptid, 0)
    ncmds = 2 if cryptid is not None else 1
    header = struct.pack("<IiIIIIII", macho_check.MH_MAGIC_64, cputype, 0, 2, ncmds, len(commands), 0, 0)
    return header + commands


def fat(*slices: tuple[int, bytes]) -> bytes:
    offset = 0x1000
    table, body = b"", b""
    for cputype, data in slices:
        table += struct.pack(">iIIII", cputype, 0, offset + len(body), len(data), 12)
        body += data + bytes(-len(data) % 0x1000)
    head = struct.pack(">II", macho_check.FAT_MAGIC, len(slices)) + table
    return head + bytes(offset - len(head)) + body


def inspect(data: bytes) -> macho_check.BinaryReport:
    return macho_check.inspect_stream(io.BytesIO(data), "bin")


class MachoCheckTest(unittest.TestCase):
    def test_thin(self):
        self.assertTrue(inspect(thin(1)).encrypted)
        self.assertFalse(inspect(thin(0)).encrypted)
        self.assertFalse(inspect(thin(None)).encrypted)
        self.assertTrue(inspect(thin(0)).arm64)

    def test_fat_picks_arm64_slice(self):
        report = inspect(fat((CPU_X86_64, thin(0, CPU_X86_64)), (macho_check.CPU_TYPE_ARM64, thin(1))))
        self.assertTrue(report.encrypted)
        self.assertTrue(report.arm64)

    def test_garbage_is_unreadable(self):
        self.assertIsNone(inspect(b"PK\x03\x04 not a binary").encrypted)
        self.assertIsNone(inspect(b"\xcf\xfa").encrypted)
        self.assertIsNone(inspect(thin(1)[:40]).encrypted)

    def test_ipa_and_app_folder(self):
        info = plistlib.dumps({"CFBundleExecutable": "Game"})
        with tempfile.TemporaryDirectory() as tmp:
            ipa = Path(tmp) / "game.ipa"
            with zipfile.ZipFile(ipa, "w", zipfile.ZIP_DEFLATED) as archive:
                archive.writestr("Payload/Game.app/Info.plist", info)
                archive.writestr("Payload/Game.app/Game", thin(0))
                archive.writestr("Payload/Game.app/Frameworks/Lib.framework/Lib", thin(1))
                archive.writestr("Payload/Game.app/Frameworks/Lib.framework/Info.plist", info)
            report = macho_check.check_ipa(ipa)
            self.assertEqual([b.name for b in report.binaries], ["Game", "Lib"])
            self.assertEqual([b.name for b in report.encrypted], ["Lib"])
            with zipfile.ZipFile(ipa) as archive:
                archive.extractall(tmp)
            folder = macho_check.check_app(Path(tmp) / "Payload" / "Game.app")
            self.assertEqual([b.encrypted for b in folder.binaries], [False, True])


if __name__ == "__main__":
    unittest.main()
