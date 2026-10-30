#!/usr/bin/env python3
"""Generates the APK's launcher label and icon from the configured game's Info.plist.

    PKG=$(python tools/apk_branding.py <res-dir> <manifest-template> <manifest-out>)

Writes values/strings.xml (app_name) and, when the bundle ships a loose AppIcon PNG, an adaptive
launcher icon into <res-dir>. The manifest copy gets the package com.xnuroid.<CFBundleName>, which
is also printed to stdout; status messages go to stderr. iOS icons are usually Apple CgBI PNGs (BGRA, premultiplied alpha,
headerless deflate), which aapt2 rejects, so they are rewritten as standard PNGs. Without a
configured bundle the label falls back to "xnuroid", the package to com.xnuroid.loader, and the
icon attribute is dropped.
"""
from __future__ import annotations

import plistlib
import re
import struct
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import game_config  # noqa: E402

FALLBACK_LABEL = "xnuroid"
PACKAGE_PREFIX = "com.xnuroid."
FALLBACK_PACKAGE_NAME = "loader"
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
ICON_ATTR = '        android:icon="@mipmap/ic_launcher"\n'

ADAPTIVE_ICON = """<?xml version="1.0" encoding="utf-8"?>
<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">
    <background android:drawable="@android:color/black" />
    <foreground>
        <inset android:drawable="@drawable/ic_launcher_art" android:inset="16.667%" />
    </foreground>
</adaptive-icon>
"""


def png_chunks(data: bytes):
    if not data.startswith(PNG_SIGNATURE):
        raise ValueError("not a PNG")
    pos = len(PNG_SIGNATURE)
    while pos + 8 <= len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        yield kind, data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IEND":
            break


def png_width(path: Path) -> int:
    for kind, body in png_chunks(path.read_bytes()):
        if kind == b"IHDR":
            return struct.unpack(">I", body[:4])[0]
    return 0


def inflate_cgbi(stream: bytes) -> bytes:
    # iDOT-split images hold several back-to-back raw deflate streams.
    out = bytearray()
    while stream:
        d = zlib.decompressobj(-15)
        out += d.decompress(stream)
        stream = d.unused_data
    return bytes(out)


def unfilter(raw: bytes, width: int, height: int, bpp: int) -> bytearray:
    stride = width * bpp
    pixels = bytearray(stride * height)
    prev = bytearray(stride)
    pos = 0
    for y in range(height):
        ftype = raw[pos]
        line = bytearray(raw[pos + 1:pos + 1 + stride])
        pos += 1 + stride
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b = prev[x]
            if ftype == 1:
                line[x] = (line[x] + a) & 0xFF
            elif ftype == 2:
                line[x] = (line[x] + b) & 0xFF
            elif ftype == 3:
                line[x] = (line[x] + ((a + b) >> 1)) & 0xFF
            elif ftype == 4:
                c = prev[x - bpp] if x >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 0xFF
        pixels[y * stride:(y + 1) * stride] = line
        prev = line
    return pixels


def png_chunk(kind: bytes, body: bytes) -> bytes:
    return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))


def normalize_png(data: bytes) -> bytes:
    chunks = list(png_chunks(data))
    if not any(kind == b"CgBI" for kind, _ in chunks):
        return data
    ihdr = next(body for kind, body in chunks if kind == b"IHDR")
    width, height, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", ihdr)
    if depth != 8 or ctype not in (2, 6) or interlace:
        raise ValueError(f"unsupported CgBI layout depth={depth} type={ctype} interlace={interlace}")
    bpp = 4 if ctype == 6 else 3
    raw = inflate_cgbi(b"".join(body for kind, body in chunks if kind == b"IDAT"))
    px = unfilter(raw, width, height, bpp)
    for i in range(0, len(px), bpp):
        px[i], px[i + 2] = px[i + 2], px[i]
        if bpp == 4:
            alpha = px[i + 3]
            if 0 < alpha < 255:
                for c in range(3):
                    px[i + c] = min(255, (px[i + c] * 255 + alpha // 2) // alpha)
    stride = width * bpp
    rows = b"".join(b"\x00" + px[y * stride:(y + 1) * stride] for y in range(height))
    return (PNG_SIGNATURE
            + png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, ctype, 0, 0, 0))
            + png_chunk(b"IDAT", zlib.compress(rows, 9))
            + png_chunk(b"IEND", b""))


def icon_names(info: dict) -> list[str]:
    names = []
    for key in ("CFBundleIcons", "CFBundleIcons~ipad"):
        names += info.get(key, {}).get("CFBundlePrimaryIcon", {}).get("CFBundleIconFiles", [])
    names += info.get("CFBundleIconFiles", [])
    if info.get("CFBundleIconFile"):
        names.append(info["CFBundleIconFile"])
    return [n[:-4] if n.lower().endswith(".png") else n for n in names] or ["AppIcon", "Icon"]


def largest_icon(bundle: Path, info: dict) -> Path | None:
    best, best_width = None, 0
    for name in dict.fromkeys(icon_names(info)):
        for path in bundle.glob(f"{name}*.png"):
            try:
                width = png_width(path)
            except (OSError, ValueError):
                continue
            if width > best_width:
                best, best_width = path, width
    return best


def android_string(text: str) -> str:
    text = text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
    text = text.replace("\\", "\\\\").replace("'", "\\'").replace('"', '\\"')
    return "\\" + text if text[:1] in ("@", "?") else text


def package_id(info: dict) -> str:
    source = info.get("CFBundleName") or info.get("CFBundleExecutable") or info.get("CFBundleDisplayName") or ""
    name = re.sub(r"[^a-z0-9_]", "", source.lower())
    if name and not name[0].isalpha():
        name = "app" + name
    return PACKAGE_PREFIX + (name or FALLBACK_PACKAGE_NAME)


def log(message: str) -> None:
    print(f"branding: {message}", file=sys.stderr)


def load_bundle() -> tuple[Path | None, dict]:
    try:
        bundle = game_config.app_bundle_dir()
    except RuntimeError as e:
        log(str(e))
        return None, {}
    try:
        with (bundle / "Info.plist").open("rb") as f:
            return bundle, plistlib.load(f)
    except (OSError, plistlib.InvalidFileException) as e:
        log(f"cannot read {bundle / 'Info.plist'}: {e}")
        return None, {}


def main() -> int:
    res, template, manifest_out = (Path(a) for a in sys.argv[1:4])
    bundle, info = load_bundle()
    label = (info.get("CFBundleDisplayName") or info.get("CFBundleName")
             or info.get("CFBundleExecutable") or FALLBACK_LABEL).strip() or FALLBACK_LABEL

    (res / "values").mkdir(parents=True, exist_ok=True)
    (res / "values" / "strings.xml").write_text(
        '<?xml version="1.0" encoding="utf-8"?>\n<resources>\n'
        f'    <string name="app_name">{android_string(label)}</string>\n</resources>\n',
        encoding="utf-8")

    icon = largest_icon(bundle, info) if bundle else None
    if icon:
        try:
            art = normalize_png(icon.read_bytes())
        except (OSError, ValueError, zlib.error, StopIteration) as e:
            log(f"skipping icon {icon.name}: {e}")
            icon = None
    if icon:
        (res / "drawable-nodpi").mkdir(parents=True, exist_ok=True)
        (res / "drawable-nodpi" / "ic_launcher_art.png").write_bytes(art)
        (res / "mipmap-anydpi-v26").mkdir(parents=True, exist_ok=True)
        (res / "mipmap-anydpi-v26" / "ic_launcher.xml").write_text(ADAPTIVE_ICON, encoding="utf-8")

    package = package_id(info)
    manifest = template.read_text(encoding="utf-8")
    if ICON_ATTR not in manifest:
        raise SystemExit(f"branding: {template} lacks the expected icon attribute line")
    manifest, count = re.subn(r'\bpackage="[^"]*"', f'package="{package}"', manifest, count=1)
    if not count:
        raise SystemExit(f"branding: {template} lacks a package attribute")

    # Match the activity's orientation to the app's UISupportedInterfaceOrientations so landscape games fill the
    # screen and portrait apps stand upright, instead of a hardcoded orientation.
    orients = info.get("UISupportedInterfaceOrientations") or []
    if isinstance(orients, str):
        orients = [orients]
    has_land = any("Landscape" in o for o in orients)
    has_port = any("Portrait" in o for o in orients)
    orientation = "sensorLandscape" if has_land and not has_port else "sensorPortrait" if has_port and not has_land else "fullSensor"
    manifest = re.sub(r'android:screenOrientation="[^"]*"', f'android:screenOrientation="{orientation}"', manifest, count=1)

    manifest_out.write_text(manifest if icon else manifest.replace(ICON_ATTR, ""), encoding="utf-8")

    log(f"package {package}, label '{label}', orientation {orientation}, icon {f'{icon.name} ({png_width(icon)}px)' if icon else 'default'}")
    print(package)
    return 0


if __name__ == "__main__":
    sys.exit(main())
