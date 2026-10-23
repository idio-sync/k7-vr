"""Search Killer7Win.exe's code.

    python tools/xref.py disp 0xc18 [--write]     instructions using [reg + 0xc18] (as destination with --write)
    python tools/xref.py addr 0x86e32c            instructions referencing an absolute address
    python tools/xref.py float 0.0174532925 [--tol 1e-7]   image locations holding the float, and code reading them
    python tools/xref.py calls RVA                call sites of the function at RVA
"""
import argparse
import struct

import capstone
import pefile

EXE = r"C:\Users\Virtual\killer7-vr-game\Killer7Win.exe"


def text_instructions(pe):
    base = pe.OPTIONAL_HEADER.ImageBase
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    md.skipdata = True
    for s in pe.sections:
        if not (s.Characteristics & 0x20000000):  # executable
            continue
        data = s.get_data()
        yield from md.disasm(data, base + s.VirtualAddress)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=["disp", "addr", "float", "calls"])
    ap.add_argument("value")
    ap.add_argument("--write", action="store_true")
    ap.add_argument("--tol", type=float, default=1e-7)
    ap.add_argument("--exe", default=EXE)
    a = ap.parse_args()
    pe = pefile.PE(a.exe)
    base = pe.OPTIONAL_HEADER.ImageBase
    targets = set()
    if a.mode == "float":
        want = float(a.value)
        for s in pe.sections:
            data = s.get_data()
            for i in range(0, len(data) - 3, 4):
                f = struct.unpack_from("<f", data, i)[0]
                if abs(f - want) <= a.tol:
                    targets.add(base + s.VirtualAddress + i)
            for i in range(0, len(data) - 7, 4):
                d = struct.unpack_from("<d", data, i)[0]
                if abs(d - want) <= a.tol:
                    targets.add(base + s.VirtualAddress + i)
        print("locations:", " ".join(f"rva {t - base:x}" for t in sorted(targets)))
    value = int(a.value, 16) if a.mode in ("disp", "addr", "calls") else None
    if a.mode == "calls":
        value += base
    for ins in text_instructions(pe):
        if ins.id == 0:
            continue
        hit = False
        for idx, op in enumerate(ins.operands):
            if op.type == capstone.x86.X86_OP_MEM:
                disp = op.mem.disp & 0xFFFFFFFF
                if a.mode == "disp" and op.mem.base != 0 and disp == value:
                    hit = not a.write or idx == 0
                elif a.mode == "addr" and op.mem.base == 0 and disp == value:
                    hit = True
                elif a.mode == "float" and op.mem.base == 0 and disp in targets:
                    hit = True
            elif op.type == capstone.x86.X86_OP_IMM:
                if a.mode == "addr" and (op.imm & 0xFFFFFFFF) == value:
                    hit = True
                elif a.mode == "calls" and ins.mnemonic in ("call", "jmp") and op.imm == value:
                    hit = True
        if hit:
            print(f"{ins.address - base:6x}: {ins.mnemonic} {ins.op_str}")


if __name__ == "__main__":
    main()
