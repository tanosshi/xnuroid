# IPA Mach-O inspector — milestone 1

This is a host-side, read-only inventory tool for **thin, little-endian arm64 Mach-O** files. It validates the header/load-command structure and reports CPU subtype, encryption metadata, chained pointer formats/import histogram, classic bind-stream presence, Objective-C reference-section slot counts, and initializer counts. It does not decrypt, map, relocate, or execute a binary.

## Build

```sh
cmake -S . -B build
cmake --build build --config Release
```

## Use

```sh
macho-inspect path/to/UnityFramework
```

The tool intentionally rejects universal/fat files; inspect a thin arm64 slice. `cryptid != 0` is reported as encrypted and causes a nonzero exit. A missing encryption command is reported as “not declared”, not assumed decrypted. Classic bind opcode streams are detected and sized but not decoded into symbol histograms. Instruction-level checks such as `svc #0x80` and `TPIDRRO_EL0` require a disassembler and are not inferred by scanning raw bytes. Objective-C section counts are pointer slots, not distinct class/selector names.

## Next milestone

The separate portable `src/chained_fixups.hpp` API accepts a bounded fixup payload, segment layout, numeric preferred/runtime bases, an `ImageSink`, and an import resolver. It currently handles `DYLD_CHAINED_PTR_64` and `DYLD_CHAINED_PTR_64_OFFSET`, plus import table formats 1, 2, and 3. ARM64e pointer-authentication formats, compressed symbol pools, and classic dyld opcodes are rejected explicitly. The inventory executable only reads fixup metadata; it does not call this relocation API.

Add a synthetic Mach-O fixture generated from our own source and compare the resulting fixup records with Apple's `dyld_info` or LLVM output. Keep parsing separate from any eventual Android memory mapper.
