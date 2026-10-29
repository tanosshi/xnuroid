"""Feasibility scan of a Unity iOS Data folder: engine version, shader container formats, audio, asset sizes.

Usage: python scan_data.py <Data folder>
Looks for: Unity version string in globalgamemanagers, Metal shader markers (MSL source text, 'MTLB' metallib magic),
GLSL/ESSL markers, FSB5 audio banks, UnityFS bundle count. Raw/compressed blobs hide text, so zero hits are inconclusive.
"""
import os
import re
import sys

MARKERS = {
    "msl_include": b"#include <metal_stdlib>",
    "msl_namespace": b"using namespace metal;",
    "metallib_MTLB": b"MTLB",
    "glsl_version": b"#version 300 es",
    "fsb5": b"FSB5",
    "unityfs": b"UnityFS",
}


def main():
    root = sys.argv[1]
    ver = None
    totals = {k: 0 for k in MARKERS}
    per_file = []
    nfiles = 0
    for dp, _, fs in os.walk(root):
        for f in fs:
            p = os.path.join(dp, f)
            nfiles += 1
            try:
                size = os.path.getsize(p)
                if size > 400 * 1024 * 1024:
                    continue
                with open(p, "rb") as fh:
                    data = fh.read()
            except OSError:
                continue
            if ver is None and f == "globalgamemanagers":
                m = re.search(rb"\d{4}\.\d+\.\d+[fpb]\d+", data[:4096])
                if m:
                    ver = m.group(0).decode()
            hits = {k: data.count(v) for k, v in MARKERS.items()}
            for k, v in hits.items():
                totals[k] += v
            if hits["msl_include"] or hits["metallib_MTLB"] > 3 or hits["fsb5"]:
                per_file.append((os.path.relpath(p, root), size, hits))
    print(f"files scanned: {nfiles}")
    print(f"unity version (globalgamemanagers header): {ver}")
    for k, v in totals.items():
        print(f"  {k}: {v}")
    print("files with shader/audio markers:")
    for rel, size, h in sorted(per_file, key=lambda x: -x[1])[:25]:
        print(f"  {rel} ({size} bytes) " + " ".join(f"{k}={v}" for k, v in h.items() if v))


main()
