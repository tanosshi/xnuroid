#!/usr/bin/env python3
"""
Path B: Desktop ARM64 Mach-O execution test harness using Unicorn Engine.
Loads arm64 Mach-O binaries, resolves dyld classic rebases and bindings,
sets up import traps and shims, and executes instructions in Unicorn.
"""

from __future__ import annotations
import os
import struct
import sys
from pathlib import Path
from typing import Callable, Dict, List, Optional, Tuple

import capstone
import unicorn
from unicorn import arm64_const as uc_arm64

from game_config import APP_EXECUTABLE, app_executable_path, guest_bundle_path


# Opcodes for classic dyld info
BIND_OPCODE_MASK = 0xF0
BIND_IMMEDIATE_MASK = 0x0F
BIND_OPCODE_DONE = 0x00
BIND_OPCODE_SET_DYLIB_ORDINAL_IMM = 0x10
BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB = 0x20
BIND_OPCODE_SET_DYLIB_SPECIAL_IMM = 0x30
BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM = 0x40
BIND_OPCODE_SET_TYPE_IMM = 0x50
BIND_OPCODE_SET_ADDEND_SLEB = 0x60
BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB = 0x70
BIND_OPCODE_ADD_ADDR_ULEB = 0x80
BIND_OPCODE_DO_BIND = 0x90
BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB = 0xA0
BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED = 0xB0
BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB = 0xC0

REBASE_OPCODE_MASK = 0xF0
REBASE_IMMEDIATE_MASK = 0x0F
REBASE_OPCODE_DONE = 0x00
REBASE_OPCODE_SET_TYPE_IMM = 0x10
REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB = 0x20
REBASE_OPCODE_ADD_ADDR_ULEB = 0x30
REBASE_OPCODE_ADD_ADDR_IMM_SCALED = 0x40
REBASE_OPCODE_DO_REBASE_IMM_TIMES = 0x50
REBASE_OPCODE_DO_REBASE_ULEB_TIMES = 0x60
REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB = 0x70
REBASE_OPCODE_DO_REBASE_ULEB_TIMES_SKIPPING_ULEB = 0x80


def read_uleb(data: bytes, cur: int) -> Tuple[int, int]:
    val = 0
    shift = 0
    while cur < len(data):
        b = data[cur]
        cur += 1
        val |= (b & 0x7F) << shift
        if (b & 0x80) == 0:
            break
        shift += 7
    return cur, val


def read_sleb(data: bytes, cur: int) -> Tuple[int, int]:
    val = 0
    shift = 0
    while cur < len(data):
        b = data[cur]
        cur += 1
        val |= (b & 0x7F) << shift
        shift += 7
        if (b & 0x80) == 0:
            if shift < 64 and (b & 0x40):
                val |= (~0 << shift)
            break
    return cur, val


def align_up(val: int, align: int = 4096) -> int:
    return (val + align - 1) & ~(align - 1)


class Segment:
    def __init__(self, name: str, vmaddr: int, vmsize: int, fileoff: int, filesize: int, initprot: int, maxprot: int):
        self.name = name
        self.vmaddr = vmaddr
        self.vmsize = vmsize
        self.fileoff = fileoff
        self.filesize = filesize
        self.initprot = initprot
        self.maxprot = maxprot


class Binding:
    def __init__(self, segment_index: int, offset: int, symbol: str, addend: int, ordinal: int):
        self.segment_index = segment_index
        self.offset = offset
        self.symbol = symbol
        self.addend = addend
        self.ordinal = ordinal


class MachOBinary:
    def __init__(self, path: Path):
        self.path = path
        self.data = path.read_bytes()
        self.segments: List[Segment] = []
        self.entry_point: Optional[int] = None
        self.rebases: List[Tuple[int, int]] = []
        self.bindings: List[Binding] = []
        self.dylibs: List[str] = []
        self.mod_init_funcs: List[int] = []
        self._parse()

    def _parse(self):
        magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags, reserved = struct.unpack_from(
            "<IIIIIIII", self.data, 0
        )
        if magic != 0xFEEDFACF:
            raise ValueError(f"Not a 64-bit Mach-O binary (magic={hex(magic)})")
        if cputype != 0x0100000C:
            raise ValueError(f"Not arm64 (cputype={hex(cputype)})")

        offset = 32
        rebase_info = None
        bind_info = None
        lazy_bind_info = None

        for _ in range(ncmds):
            cmd, cmdsize = struct.unpack_from("<II", self.data, offset)
            if cmd == 0x19:  # LC_SEGMENT_64
                segname = self.data[offset + 8 : offset + 24].split(b"\x00", 1)[0].decode("ascii", "ignore")
                vmaddr, vmsize, fileoff, filesize, maxprot, initprot, nsects, segflags = struct.unpack_from(
                    "<QQQQIIII", self.data, offset + 24
                )
                seg = Segment(segname, vmaddr, vmsize, fileoff, filesize, initprot, maxprot)
                self.segments.append(seg)

                # Check sections inside segment
                sect_offset = offset + 72
                for _ in range(nsects):
                    sname = self.data[sect_offset : sect_offset + 16].split(b"\x00", 1)[0].decode("ascii", "ignore")
                    saddr, ssize, soff, salign, sreloff, snrel, sflags, _res = struct.unpack_from(
                        "<QQIIIIII", self.data, sect_offset + 32
                    )
                    if sname == "__mod_init_func":
                        count = ssize // 8
                        for p_idx in range(count):
                            func_ptr = struct.unpack_from("<Q", self.data, soff + p_idx * 8)[0]
                            self.mod_init_funcs.append(func_ptr)
                    sect_offset += 80

            elif cmd == 0x80000028:  # LC_MAIN
                entryoff, stacksize = struct.unpack_from("<QQ", self.data, offset + 8)
                self.entry_point = entryoff

            elif cmd in (0x22, 0x80000022):  # LC_DYLD_INFO / LC_DYLD_INFO_ONLY
                r_off, r_size, b_off, b_size, w_off, w_size, l_off, l_size, e_off, e_size = struct.unpack_from(
                    "<IIIIIIIIII", self.data, offset + 8
                )
                rebase_info = (r_off, r_size)
                bind_info = (b_off, b_size)
                lazy_bind_info = (l_off, l_size)

            elif cmd in (0x0C, 0x80000018, 0x8000001F, 0x80000023, 0x20):
                str_off = struct.unpack_from("<I", self.data, offset + 8)[0]
                lib_name = self.data[offset + str_off : offset + cmdsize].split(b"\x00", 1)[0].decode("utf-8", "ignore")
                self.dylibs.append(lib_name)

            offset += cmdsize

        if rebase_info and rebase_info[1] > 0:
            self._parse_rebases(rebase_info[0], rebase_info[1])
        if bind_info and bind_info[1] > 0:
            self._parse_binds(bind_info[0], bind_info[1], is_lazy=False)
        if lazy_bind_info and lazy_bind_info[1] > 0:
            self._parse_binds(lazy_bind_info[0], lazy_bind_info[1], is_lazy=True)

    def _parse_rebases(self, offset: int, size: int):
        p = self.data[offset : offset + size]
        cur = 0
        seg_idx = 0
        addr = 0
        while cur < len(p):
            byte = p[cur]
            cur += 1
            op = byte & REBASE_OPCODE_MASK
            imm = byte & REBASE_IMMEDIATE_MASK
            if op == REBASE_OPCODE_DONE:
                break
            elif op == REBASE_OPCODE_SET_TYPE_IMM:
                pass
            elif op == REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
                seg_idx = imm
                cur, addr = read_uleb(p, cur)
            elif op == REBASE_OPCODE_ADD_ADDR_ULEB:
                cur, u = read_uleb(p, cur)
                addr = (addr + u) & 0xFFFFFFFFFFFFFFFF
            elif op == REBASE_OPCODE_ADD_ADDR_IMM_SCALED:
                addr = (addr + imm * 8) & 0xFFFFFFFFFFFFFFFF
            elif op == REBASE_OPCODE_DO_REBASE_IMM_TIMES:
                for _ in range(imm):
                    self.rebases.append((seg_idx, addr))
                    addr = (addr + 8) & 0xFFFFFFFFFFFFFFFF
            elif op == REBASE_OPCODE_DO_REBASE_ULEB_TIMES:
                cur, count = read_uleb(p, cur)
                for _ in range(count):
                    self.rebases.append((seg_idx, addr))
                    addr = (addr + 8) & 0xFFFFFFFFFFFFFFFF
            elif op == REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB:
                self.rebases.append((seg_idx, addr))
                cur, u = read_uleb(p, cur)
                addr = (addr + u + 8) & 0xFFFFFFFFFFFFFFFF
            elif op == REBASE_OPCODE_DO_REBASE_ULEB_TIMES_SKIPPING_ULEB:
                cur, count = read_uleb(p, cur)
                cur, skip = read_uleb(p, cur)
                for _ in range(count):
                    self.rebases.append((seg_idx, addr))
                    addr = (addr + skip + 8) & 0xFFFFFFFFFFFFFFFF

    def _parse_binds(self, offset: int, size: int, is_lazy: bool):
        p = self.data[offset : offset + size]
        cur = 0
        seg_idx = 0
        addr = 0
        ordinal = 0
        symbol = ""
        addend = 0

        while cur < len(p):
            byte = p[cur]
            cur += 1
            op = byte & BIND_OPCODE_MASK
            imm = byte & BIND_IMMEDIATE_MASK

            if op == BIND_OPCODE_DONE:
                if not is_lazy:
                    break
                addr = 0
                ordinal = 0
                symbol = ""
                addend = 0
                seg_idx = 0
            elif op == BIND_OPCODE_SET_DYLIB_ORDINAL_IMM:
                ordinal = imm
            elif op == BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB:
                cur, ordinal = read_uleb(p, cur)
            elif op == BIND_OPCODE_SET_DYLIB_SPECIAL_IMM:
                ordinal = 0 if imm == 0 else (imm - 16)
            elif op == BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM:
                start = cur
                while cur < len(p) and p[cur] != 0:
                    cur += 1
                symbol = p[start:cur].decode("utf-8", "ignore")
                cur += 1
            elif op == BIND_OPCODE_SET_TYPE_IMM:
                pass
            elif op == BIND_OPCODE_SET_ADDEND_SLEB:
                cur, addend = read_sleb(p, cur)
            elif op == BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
                seg_idx = imm
                cur, addr = read_uleb(p, cur)
            elif op == BIND_OPCODE_ADD_ADDR_ULEB:
                cur, u = read_uleb(p, cur)
                addr = (addr + u) & 0xFFFFFFFFFFFFFFFF
            elif op == BIND_OPCODE_DO_BIND:
                self.bindings.append(Binding(seg_idx, addr, symbol, addend, ordinal))
                addr = (addr + 8) & 0xFFFFFFFFFFFFFFFF
            elif op == BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB:
                cur, u = read_uleb(p, cur)
                self.bindings.append(Binding(seg_idx, addr, symbol, addend, ordinal))
                addr = (addr + u + 8) & 0xFFFFFFFFFFFFFFFF
            elif op == BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED:
                self.bindings.append(Binding(seg_idx, addr, symbol, addend, ordinal))
                addr = (addr + imm * 8 + 8) & 0xFFFFFFFFFFFFFFFF
            elif op == BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB:
                cur, count = read_uleb(p, cur)
                cur, skip = read_uleb(p, cur)
                for _ in range(count):
                    self.bindings.append(Binding(seg_idx, addr, symbol, addend, ordinal))
                    addr = (addr + skip + 8) & 0xFFFFFFFFFFFFFFFF


class ExecutionHarness:
    TRAMPOLINE_BASE = 0x7000_0000_0000
    STACK_BASE = 0x7FFF_0000_0000
    STACK_SIZE = 2 * 1024 * 1024  # 2MB
    HEAP_BASE = 0x6000_0000_0000
    HEAP_SIZE = 16 * 1024 * 1024  # 16MB

    def __init__(self, verbose: bool = True):
        self.verbose = verbose
        self.uc = unicorn.Uc(unicorn.UC_ARCH_ARM64, unicorn.UC_MODE_ARM)
        self.cs = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
        self.symbol_to_trampoline: Dict[str, int] = {}
        self.trampoline_to_symbol: Dict[int, str] = {}
        self.custom_handlers: Dict[str, Callable[[ExecutionHarness], Optional[int]]] = {}
        self.heap_ptr = self.HEAP_BASE + 0x1000
        self.trace_count = 0
        self.max_trace = 10000
        self.call_log: List[str] = []
        self.on_trap: Optional[Callable[[str, int, int, int, int], None]] = None
        self._setup_system_memory()

    def _setup_system_memory(self):
        # Map Stack
        self.uc.mem_map(self.STACK_BASE, self.STACK_SIZE, unicorn.UC_PROT_READ | unicorn.UC_PROT_WRITE)
        sp = self.STACK_BASE + self.STACK_SIZE - 0x2000
        self.uc.reg_write(uc_arm64.UC_ARM64_REG_SP, sp)

        # Map Heap / Data
        self.uc.mem_map(self.HEAP_BASE, self.HEAP_SIZE, unicorn.UC_PROT_READ | unicorn.UC_PROT_WRITE)

        # Map Trampolines
        self.uc.mem_map(self.TRAMPOLINE_BASE, 0x100000, unicorn.UC_PROT_READ | unicorn.UC_PROT_EXEC)

        # Hook Trampoline range
        self.uc.hook_add(
            unicorn.UC_HOOK_CODE,
            self._on_trampoline_hit,
            begin=self.TRAMPOLINE_BASE,
            end=self.TRAMPOLINE_BASE + 0x100000,
        )

        # Set up TLS
        self.uc.reg_write(uc_arm64.UC_ARM64_REG_TPIDRRO_EL0, self.HEAP_BASE + 0x100)
        self.uc.reg_write(uc_arm64.UC_ARM64_REG_TPIDR_EL0, self.HEAP_BASE + 0x100)

    def allocate_string(self, s: str) -> int:
        encoded = s.encode("utf-8") + b"\x00"
        addr = self.heap_ptr
        self.heap_ptr = align_up(self.heap_ptr + len(encoded), 16)
        self.uc.mem_write(addr, encoded)
        return addr

    def allocate_cfstring(self, s: str) -> int:
        """Create a mock CFString structure in memory."""
        str_addr = self.allocate_string(s)
        # CFString layout: isa (8), info (8), data_ptr (8), length (8)
        cf_addr = self.heap_ptr
        self.heap_ptr = align_up(self.heap_ptr + 32, 16)
        cf_class_ref = self.get_or_create_trampoline("___CFConstantStringClassReference")
        self.uc.mem_write(cf_addr, struct.pack("<QQQQ", cf_class_ref, 0x07C8, str_addr, len(s)))
        return cf_addr

    def read_string(self, addr: int, max_len: int = 256) -> str:
        try:
            raw = self.uc.mem_read(addr, max_len)
            nul = raw.find(b"\x00")
            if nul != -1:
                raw = raw[:nul]
            return raw.decode("utf-8", "ignore")
        except Exception:
            return f"<invalid 0x{addr:x}>"

    def get_or_create_trampoline(self, symbol: str) -> int:
        if symbol in self.symbol_to_trampoline:
            return self.symbol_to_trampoline[symbol]

        idx = len(self.symbol_to_trampoline)
        t_addr = self.TRAMPOLINE_BASE + idx * 16
        self.symbol_to_trampoline[symbol] = t_addr
        self.trampoline_to_symbol[t_addr] = symbol

        # Write ARM64 'ret' instruction: 0xd65f03c0
        self.uc.mem_write(t_addr, struct.pack("<I", 0xD65F03C0))
        return t_addr

    def register_handler(self, symbol: str, handler: Callable[[ExecutionHarness], Optional[int]]):
        self.custom_handlers[symbol] = handler

    def _on_trampoline_hit(self, uc: unicorn.Uc, address: int, size: int, user_data):
        symbol = self.trampoline_to_symbol.get(address, f"<unknown_0x{address:x}>")
        x0 = uc.reg_read(uc_arm64.UC_ARM64_REG_X0)
        x1 = uc.reg_read(uc_arm64.UC_ARM64_REG_X1)
        x2 = uc.reg_read(uc_arm64.UC_ARM64_REG_X2)
        x3 = uc.reg_read(uc_arm64.UC_ARM64_REG_X3)
        lr = uc.reg_read(uc_arm64.UC_ARM64_REG_LR)

        call_desc = f"[SHIM TRAP] {symbol}(x0={hex(x0)}, x1={hex(x1)}, x2={hex(x2)}, x3={hex(x3)}) -> return to LR={hex(lr)}"
        self.call_log.append(call_desc)
        if self.on_trap:
            self.on_trap(symbol, x0, x1, x2, x3)
        if self.verbose:
            print(call_desc)

        ret_val = None
        if symbol in self.custom_handlers:
            ret_val = self.custom_handlers[symbol](self)
        else:
            # Default dispatch
            ret_val = self._default_shim(symbol, x0, x1, x2, x3)

        if ret_val is not None:
            uc.reg_write(uc_arm64.UC_ARM64_REG_X0, ret_val)

        # Set PC to LR to simulate 'ret' (only if handler didn't redirect PC)
        current_pc = uc.reg_read(uc_arm64.UC_ARM64_REG_PC)
        if current_pc == address:
            uc.reg_write(uc_arm64.UC_ARM64_REG_PC, lr)

    def _default_shim(self, symbol: str, x0: int, x1: int, x2: int, x3: int) -> int:
        if symbol == "_objc_autoreleasePoolPush":
            return 0x1000
        elif symbol == "_objc_autoreleasePoolPop":
            return 0
        elif symbol in ("_objc_retainAutoreleasedReturnValue", "_objc_retain", "_objc_autoreleaseReturnValue"):
            return x0
        elif symbol == "_objc_release":
            return 0
        elif symbol == "_objc_msgSend":
            sel_name = self.read_string(x1)
            if self.verbose:
                print(f"      -> _objc_msgSend: obj={hex(x0)}, selector='{sel_name}'")
            return self._handle_objc_msg_send(x0, sel_name, x2, x3)
        elif symbol == "___cxa_atexit":
            return 0
        elif symbol == "_abort":
            print("Abort called!")
            self.uc.emu_stop()
            return 0
        elif symbol == "_exit":
            print(f"Exit({x0}) called!")
            self.uc.emu_stop()
            return 0
        return 0

    def _handle_objc_msg_send(self, obj: int, selector: str, arg1: int, arg2: int) -> int:
        bundle_class_addr = self.symbol_to_trampoline.get("_OBJC_CLASS_$_NSBundle", 0)
        unity_class_addr = self.symbol_to_trampoline.get("_OBJC_CLASS_$_UnityFramework", 0)

        if selector == "mainBundle":
            return 0x2000  # mock mainBundle instance
        elif selector == "bundlePath":
            return self.allocate_cfstring(guest_bundle_path())
        elif selector == "stringByAppendingString:":
            suffix = self.read_cfstring(arg1)
            new_path = guest_bundle_path() + suffix
            if self.verbose:
                print(f"         [App appended path: '{new_path}']")
            return self.allocate_cfstring(new_path)
        elif selector == "bundleWithPath:":
            return 0x2001  # mock framework bundle instance
        elif selector == "isLoaded":
            return 1
        elif selector == "load":
            if self.verbose:
                print("         [Bundle load requested -> SUCCESS]")
            return 1
        elif selector == "principalClass":
            return unity_class_addr or 0x3000
        elif selector == "getInstance":
            if self.verbose:
                print("         [UnityFramework getInstance -> Returning singleton 0x3001]")
            return 0x3001
        elif selector == "appController":
            if self.verbose:
                print("         [UnityFramework appController -> Returning controller 0x4000]")
            return 0x4000
        elif selector == "setExecuteHeader:":
            if self.verbose:
                print(f"         [UnityFramework setExecuteHeader: {hex(arg1)}]")
            return 0
        elif selector == "runUIApplicationMainWithArgc:argv:":
            if self.verbose:
                print(f"         [*] [UnityFramework runUIApplicationMainWithArgc: {arg1} argv: {hex(arg2)}]")
                print("         [*] >>> IPA LAUNCH SEQUENCE COMPLETED SUCCESSFULLY! <<<")
            self.uc.emu_stop()
            return 0
        return 0

    def read_cfstring(self, cf_addr: int) -> str:
        try:
            # CFString: isa(8), info(8), data_ptr(8), length(8)
            raw = self.uc.mem_read(cf_addr, 32)
            isa, info, data_ptr, length = struct.unpack("<QQQQ", raw)
            return self.read_string(data_ptr, length)
        except Exception:
            return ""

    def load_macho(self, macho: MachOBinary, base_slide: int = 0) -> int:
        print(f"\n[Loader] Loading Mach-O binary: {macho.path.name}")
        for seg in macho.segments:
            if seg.name == "__PAGEZERO" or seg.vmsize == 0:
                continue

            seg_vmaddr = seg.vmaddr + base_slide
            aligned_addr = seg_vmaddr & ~0xFFF
            aligned_size = align_up(seg.vmsize, 4096)

            prot = unicorn.UC_PROT_READ | unicorn.UC_PROT_WRITE
            if seg.initprot & 4:  # execute
                prot |= unicorn.UC_PROT_EXEC

            print(f"  Mapping {seg.name:14s} at {hex(aligned_addr)} - {hex(aligned_addr + aligned_size)} (prot={prot})")
            self.uc.mem_map(aligned_addr, aligned_size, prot)

            # Copy file contents
            if seg.filesize > 0:
                chunk = macho.data[seg.fileoff : seg.fileoff + seg.filesize]
                self.uc.mem_write(seg_vmaddr, chunk)

        # Apply Rebases
        if base_slide != 0 and macho.rebases:
            print(f"  Applying {len(macho.rebases)} rebases with slide {hex(base_slide)}...")
            for seg_idx, offset in macho.rebases:
                target = macho.segments[seg_idx].vmaddr + base_slide + offset
                val = struct.unpack("<Q", self.uc.mem_read(target, 8))[0]
                val = (val + base_slide) & 0xFFFFFFFFFFFFFFFF
                self.uc.mem_write(target, struct.pack("<Q", val))

        # Apply Bindings
        print(f"  Resolving {len(macho.bindings)} symbol bindings to trampolines...")
        for b in macho.bindings:
            target_addr = macho.segments[b.segment_index].vmaddr + base_slide + b.offset
            trampoline_addr = self.get_or_create_trampoline(b.symbol)
            final_val = (trampoline_addr + b.addend) & 0xFFFFFFFFFFFFFFFF
            self.uc.mem_write(target_addr, struct.pack("<Q", final_val))

        preferred_base = macho.segments[1].vmaddr if len(macho.segments) > 1 else 0
        entry = (preferred_base + base_slide + macho.entry_point) if macho.entry_point is not None else 0
        return entry

    def execute_from(self, start_addr: int, max_instructions: int = 50000):
        print(f"\n[Execution] Starting ARM64 CPU emulation at {hex(start_addr)}...")

        # Setup argc/argv
        prog_name_ptr = self.allocate_string(APP_EXECUTABLE)
        argv_ptr = self.heap_ptr
        self.heap_ptr = align_up(self.heap_ptr + 16, 16)
        self.uc.mem_write(argv_ptr, struct.pack("<QQ", prog_name_ptr, 0))

        self.uc.reg_write(uc_arm64.UC_ARM64_REG_X0, 1)  # argc = 1
        self.uc.reg_write(uc_arm64.UC_ARM64_REG_X1, argv_ptr)  # argv
        self.uc.reg_write(uc_arm64.UC_ARM64_REG_LR, 0xDEADBEEF)  # exit on return

        # Instruction trace hook
        def trace_instruction(uc: unicorn.Uc, address: int, size: int, user_data):
            self.trace_count += 1
            if self.trace_count <= 25 or self.trace_count % 1000 == 0:
                code = uc.mem_read(address, size)
                for insn in self.cs.disasm(code, address):
                    print(f"  [ARM64] {hex(insn.address)}: {insn.mnemonic:8s} {insn.op_str}")
            if self.trace_count > max_instructions:
                print("Max instructions reached, stopping.")
                uc.emu_stop()

        trace_hook = self.uc.hook_add(
            unicorn.UC_HOOK_CODE,
            trace_instruction,
            begin=0x100000000,
            end=0x100010000,
        )

        try:
            self.uc.emu_start(start_addr, 0xDEADBEEF, timeout=0, count=max_instructions)
            print(f"\n[Execution] Completed {self.trace_count} instructions successfully!")
        except unicorn.UcError as e:
            pc = self.uc.reg_read(uc_arm64.UC_ARM64_REG_PC)
            print(f"\n[Execution Stopped] Unicorn Error: {e} at PC={hex(pc)}")
        finally:
            self.uc.hook_del(trace_hook)


def run():
    app_binary_path = app_executable_path()
    if not app_binary_path.exists():
        print(f"Error: {app_binary_path} not found.")
        sys.exit(1)

    macho = MachOBinary(app_binary_path)
    harness = ExecutionHarness(verbose=True)
    entry = harness.load_macho(macho)
    print(f"[Loader] Entry point determined: {hex(entry)}")
    harness.execute_from(entry)


if __name__ == "__main__":
    run()
