"""Disassemble Killer7Win.exe around an RVA (offsets as logged by the probe: exe+RVA).

    py -I tools/disasm.py RVA [--count N] [--func] [--exe PATH]

--func backs up to the start of the containing function (after int3/nop padding or a
push ebp; mov ebp, esp prologue). Calls/jumps to imports are named; float constants loaded
from the image are shown.
"""
import argparse
import struct

import capstone
import pefile

DEFAULT_EXE = r"C:\Users\Virtual\killer7-vr-game\Killer7Win.exe"


def load(path):
    pe = pefile.PE(path, fast_load=False)
    base = pe.OPTIONAL_HEADER.ImageBase
    imports = {}
    for entry in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []):
        for imp in entry.imports:
            name = imp.name.decode() if imp.name else f"ord{imp.ordinal}"
            imports[imp.address] = f"{entry.dll.decode()}!{name}"
    return pe, base, imports


def read(pe, rva, size):
    return pe.get_data(rva, size)


def func_start(pe, rva):
    data = read(pe, max(0, rva - 0x4000), min(rva, 0x4000))
    start = rva - len(data)
    for i in range(len(data) - 1, 2, -1):
        # int3/nop padding followed by code is the usual function boundary in MSVC output
        if data[i - 1] in (0xCC, 0x90) and data[i] not in (0xCC, 0x90):
            return start + i
        if data[i - 2:i + 1] == b"\x55\x8b\xec" and (i < 3 or data[i - 3] in (0xCC, 0x90, 0xC3)):
            return start + i - 2
    return rva


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("rva", type=lambda s: int(s, 16))
    ap.add_argument("--count", type=int, default=40)
    ap.add_argument("--func", action="store_true")
    ap.add_argument("--exe", default=DEFAULT_EXE)
    a = ap.parse_args()
    pe, base, imports = load(a.exe)
    rva = func_start(pe, a.rva) if a.func else a.rva
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    code = read(pe, rva, a.count * 15)
    n = 0
    for ins in md.disasm(code, base + rva):
        note = ""
        for op in ins.operands:
            if op.type == capstone.x86.X86_OP_MEM and op.mem.base == 0 and op.mem.index == 0:
                addr = op.mem.disp & 0xFFFFFFFF
                if addr in imports:
                    note = f"  ; {imports[addr]}"
                elif base <= addr < base + pe.OPTIONAL_HEADER.SizeOfImage:
                    try:
                        raw = read(pe, addr - base, 8)
                        f = struct.unpack("<f", raw[:4])[0]
                        d = struct.unpack("<d", raw)[0]
                        note = f"  ; [rva {addr - base:x}] f={f:.6g} d={d:.6g}"
                    except Exception:
                        pass
            elif op.type == capstone.x86.X86_OP_IMM and ins.mnemonic in ("call", "jmp") or ins.mnemonic.startswith("j"):
                if op.type == capstone.x86.X86_OP_IMM:
                    note = f"  ; -> rva {op.imm - base:x}"
        marker = ">>" if ins.address - base == a.rva else "  "
        print(f"{marker} {ins.address - base:6x}: {ins.mnemonic:8s} {ins.op_str}{note}")
        n += 1
        if n >= a.count:
            break


if __name__ == "__main__":
    main()
