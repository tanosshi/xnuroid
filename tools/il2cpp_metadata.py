#!/usr/bin/env python3
"""Parse Unity's global-metadata.dat into per-assembly method name tables.

IL2CPP lays each assembly's method pointers out in *token order*, and the metadata's type/method tables are
themselves token ordered, so sorting a type's methods by token and then concatenating types in token order
reproduces the exact index order of `Il2CppCodeGenModule.methodPointers`. That is what lets a runtime address
table (dumped by the loader with LOADER_IL2CPP_MODULES) be joined to names here.

Usage:
    il2cpp_metadata.py summary <global-metadata.dat>
    il2cpp_metadata.py names   <global-metadata.dat> <assembly> [out.txt]
    il2cpp_metadata.py images  <global-metadata.dat>

`names` prints/writes one line per method: "<token> <Namespace.Type::Method>".
"""

import struct
import sys

SANITY = 0xFAB11BAF

# Header is: sanity, version, then (offset, size) for every table, in this order.
TABLES = [
    "stringLiteral", "stringLiteralData", "string", "events", "properties", "methods",
    "parameterDefaultValues", "fieldDefaultValues", "fieldAndParameterDefaultValueData",
    "fieldMarshaledSizes", "parameters", "fields", "genericParameters", "genericParameterConstraints",
    "genericContainers", "nestedTypes", "interfaces", "vtableMethods", "interfaceOffsets",
    "typeDefinitions", "images", "assemblies", "fieldRefs", "referencedAssemblies", "attributeData",
    "attributeDataRange", "unresolvedVirtualCallParameterTypes", "unresolvedVirtualCallParameterRanges",
    "windowsRuntimeTypeNames", "windowsRuntimeStrings", "exportedTypeDefinitions",
]

IMAGE_SIZE = 40
TYPE_SIZE = 88     # v29: 8 index ints, fieldStart, methodStart, 6 more ints, 8 ushort counts, 2 ints
METHOD_SIZE = 32
# v29 field offsets inside a type record. Derived against the live file by requiring each table to tile its
# range exactly: fieldStart@32 (55933 fields), methodStart@36 (80924 methods), counts from 64, token@84.
TYPE_NAME, TYPE_NAMESPACE, TYPE_FIELD_START, TYPE_METHOD_START = 0, 4, 32, 36
TYPE_METHOD_COUNT, TYPE_FIELD_COUNT, TYPE_TOKEN = 64, 68, 84


class Metadata:
    def __init__(self, data):
        self.data = data
        sanity, self.version = struct.unpack_from("<Ii", data, 0)
        if sanity != SANITY:
            raise SystemExit("not a global-metadata.dat (sanity %#x)" % sanity)
        self.table = {}
        for i, name in enumerate(TABLES):
            off, size = struct.unpack_from("<Ii", data, 8 + i * 8)
            self.table[name] = (off, size)
        self.string_cache = {}

    def u32(self, off):
        return struct.unpack_from("<i", self.data, off)[0]

    def string(self, index):
        if index in self.string_cache:
            return self.string_cache[index]
        base = self.table["string"][0] + index
        end = self.data.index(b"\0", base)
        value = self.data[base:end].decode("utf-8", "replace")
        self.string_cache[index] = value
        return value

    def images(self):
        off, size = self.table["images"]
        out = []
        for i in range(size // IMAGE_SIZE):
            p = off + i * IMAGE_SIZE
            name_index = self.u32(p)
            type_start, type_count = self.u32(p + 8), self.u32(p + 12)
            out.append({"index": i, "name": self.string(name_index),
                        "type_start": type_start, "type_count": type_count})
        return out

    def type_at(self, index):
        off = self.table["typeDefinitions"][0] + index * TYPE_SIZE
        name = self.string(self.u32(off + TYPE_NAME))
        namespace = self.string(self.u32(off + TYPE_NAMESPACE))
        method_start = self.u32(off + TYPE_METHOD_START)
        method_count = struct.unpack_from("<H", self.data, off + TYPE_METHOD_COUNT)[0]
        return {"name": name, "namespace": namespace,
                "method_start": method_start, "method_count": method_count}

    def method_at(self, index):
        off = self.table["methods"][0] + index * METHOD_SIZE
        name = self.string(self.u32(off))
        token = struct.unpack_from("<I", self.data, off + 20)[0]
        return {"name": name, "token": token}

    def image_methods(self, image):
        """(token, 'Namespace.Type::Method') for one image, in IL2CPP method-pointer order."""
        methods = []
        for t in range(image["type_start"], image["type_start"] + image["type_count"]):
            ty = self.type_at(t)
            prefix = (ty["namespace"] + "." if ty["namespace"] else "") + ty["name"]
            for m in range(ty["method_start"], ty["method_start"] + ty["method_count"]):
                md = self.method_at(m)
                methods.append((md["token"], prefix + "::" + md["name"]))
        methods.sort(key=lambda pair: pair[0])
        return methods


def load(path):
    with open(path, "rb") as handle:
        return Metadata(handle.read())


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    command, path = argv[1], argv[2]
    md = load(path)
    images = md.images()
    if command == "summary":
        print("metadata version %d, %d images" % (md.version, len(images)))
        for image in images:
            print("  %-40s types %6d  methods %6d" %
                  (image["name"], image["type_count"], len(md.image_methods(image))))
        return 0
    if command == "images":
        for image in images:
            print(image["name"])
        return 0
    if command == "names":
        if len(argv) < 4:
            print("names needs an assembly name")
            return 2
        wanted = argv[3]
        match = [i for i in images if i["name"] == wanted]
        if not match:
            print("no such assembly; try: %s" % ", ".join(i["name"] for i in images))
            return 1
        lines = ["%04x %s" % (token & 0xFFFFFF, name) for token, name in md.image_methods(match[0])]
        if len(argv) >= 5:
            with open(argv[4], "w", encoding="utf-8") as handle:
                handle.write("\n".join(lines) + "\n")
            print("%d methods -> %s" % (len(lines), argv[4]))
        else:
            print("\n".join(lines))
        return 0
    print("unknown command %r" % command)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
