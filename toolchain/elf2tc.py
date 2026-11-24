#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""elf2tc.py —— TCRV32 镜像生成器（ELF → 可装入游戏内存的二进制）

为什么不用 llvm-objcopy -O binary：
  它只按 VMA 平铺，**不检查**段是否落在合法内存区域、也不处理 IMEM/DMEM 分离，
  更不会告诉你"哪一段越界了"。本工具把这四件事一次做完：
    1. 校验 e_machine / e_flags（无 RVC、软浮点）
    2. 按 PT_LOAD 的 **p_paddr**（物理/LMA）落位 —— 这样 .data 的"加载副本在 IMEM、
       运行地址在 DMEM"才有意义
    3. 段必须落在 regions 内，且可执行段只能落在 imem
    4. 输出：原始 .bin（游戏"从文件加载"用，实测为二进制语义）+ 可选字级 hex 文本
       + 符号表 + 段报告

用法：
  python tools/elf2tc.py build/app.elf -o build/app.bin
  python tools/elf2tc.py build/app.elf -o build/app.bin --pad-to 0x100000 --hex build/app.hex
  python tools/elf2tc.py build/app.elf -o build/app.bin --sym build/app.sym --report
"""

import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import memmap

EM_RISCV = 243
EF_RISCV_RVC = 0x1
FLOAT_ABI_MASK = 0x6
PT_LOAD = 1
SHT_SYMTAB = 2
SHF_ALLOC = 0x2
SHF_EXECINSTR = 0x4

# 与 ../RV32IM-ISA规范.md §2.3 / toolchain/link.ld 一致
REGIONS = []   # 由 --map 选择的内存地图在运行时填入，见 main()
DEFAULT_MAP = memmap.DEFAULT


def region_of(addr):
    for name, base, size, ex, wr in REGIONS:
        if base <= addr < base + size:
            return name, base, size, ex, wr
    return None


class Elf:
    def __init__(self, path):
        self.data = open(path, "rb").read()
        d = self.data
        if d[:4] != b"\x7fELF":
            raise SystemExit("不是 ELF：%s" % path)
        if d[4] != 1 or d[5] != 1:
            raise SystemExit("只支持 ELF32 小端（EI_CLASS=1, EI_DATA=1）")
        (self.type, self.machine, self.version, self.entry, self.phoff,
         self.shoff, self.flags, self.ehsize, self.phentsize, self.phnum,
         self.shentsize, self.shnum, self.shstrndx) = struct.unpack_from(
            "<HHIIIIIHHHHHH", d, 16)
        self.segs, self.secs, self.syms = [], [], []
        for i in range(self.phnum):
            (p_type, p_off, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags,
             p_align) = struct.unpack_from("<IIIIIIII", d,
                                           self.phoff + i * self.phentsize)
            self.segs.append(dict(type=p_type, off=p_off, vaddr=p_vaddr,
                                  paddr=p_paddr, filesz=p_filesz,
                                  memsz=p_memsz, flags=p_flags))
        shstr = None
        raw = []
        for i in range(self.shnum):
            (nm, st, fl, addr, off, sz, link, info, al, es) = struct.unpack_from(
                "<IIIIIIIIII", d, self.shoff + i * self.shentsize)
            raw.append(dict(name_off=nm, type=st, flags=fl, addr=addr, off=off,
                            size=sz, link=link, entsize=es))
        if self.shnum and self.shstrndx < len(raw):
            s = raw[self.shstrndx]
            names = d[s["off"]:s["off"] + s["size"]]
            for r in raw:
                r["name"] = names[r["name_off"]:].split(b"\x00", 1)[0].decode(
                    "utf-8", "replace")
            shstr = names
        self.secs = raw
        for r in raw:
            if r["type"] == SHT_SYMTAB and r.get("name"):
                stride = r["entsize"] or 16
                strs = raw[r["link"]]["off"] if r["link"] < len(raw) else 0
                for k in range(r["size"] // stride):
                    nm, val, sz, info, other, shndx = struct.unpack_from(
                        "<IIIBBH", d, r["off"] + k * stride)
                    end = d.index(b"\x00", strs + nm)
                    name = d[strs + nm:end].decode("utf-8", "replace")
                    if name:
                        self.syms.append((val, sz, info & 0xF, name))


def build(elf, base=0, pad_to=None, verbose=False):
    problems = []
    if elf.machine != EM_RISCV:
        problems.append("e_machine=%d 不是 RISC-V(243)" % elf.machine)
    if elf.flags & EF_RISCV_RVC:
        problems.append("e_flags 含 RVC：目标混入 16 位指令（用 -march=rv32im 重编）")
    if (elf.flags & FLOAT_ABI_MASK):
        problems.append("浮点 ABI 位非 soft：应 -mabi=ilp32")

    spans = []
    for s in elf.segs:
        if s["type"] != PT_LOAD or s["filesz"] == 0:
            continue
        spans.append((s["paddr"], bytes(elf.data[s["off"]:s["off"] + s["filesz"]]),
                      s["flags"], s["memsz"]))
    if not spans:
        problems.append("没有任何 PT_LOAD 段")

    # 段地址合法性
    for paddr, blob, flags, memsz in spans:
        lo, hi = paddr, paddr + memsz - 1
        for probe in (lo, hi):
            r = region_of(probe)
            if r is None:
                problems.append("段 [0x%08X..0x%08X] 跨出合法区域（probe 0x%08X）"
                                % (lo, hi, probe))
                break
        else:
            r = region_of(lo)
            if (flags & 0x1) and not r[3]:          # PF_X
                problems.append("可执行段 0x%08X 落在 %s（应落在 imem）" % (lo, r[0]))
        if paddr % 4:
            problems.append("段起始 0x%08X 未 4 字节对齐" % paddr)

    # RVC 扫描只针对**可执行节**：.rodata 常常和 .text 在同一个 PT_LOAD 里（合法），
    # 把它当指令扫描会误报。这一点与 ../tools/rvcheck.py 的口径一致。
    for sec in elf.secs:
        if sec["type"] != 1 or not (sec["flags"] & SHF_EXECINSTR) \
                or not (sec["flags"] & SHF_ALLOC):
            continue
        blob = elf.data[sec["off"]:sec["off"] + sec["size"]]
        for i in range(0, len(blob) - 3, 4):
            w = int.from_bytes(blob[i:i + 4], "little")
            if w & 3 != 3:
                problems.append("%s 偏移 0x%X 处不是 32 位指令字（0x%08X）"
                                % (sec.get("name", "?"), i, w))
                break

    end = max(p + len(b) for p, b, _, _ in spans)
    size = pad_to if pad_to else end
    if size < end:
        problems.append("--pad-to 0x%X 小于镜像末端 0x%X" % (size, end))
        size = end
    img = bytearray(size)
    for paddr, blob, _, _ in spans:
        if paddr + len(blob) > size:
            problems.append("段 0x%08X+%d 超出输出尺寸 0x%X" % (paddr, len(blob), size))
            continue
        img[paddr:paddr + len(blob)] = blob

    if verbose:
        print("%-8s %-10s %-10s %-10s" % ("段", "物理/LMA", "文件长", "内存长"))
        for paddr, blob, flags, memsz in sorted(spans, key=lambda t: t[0]):
            r = region_of(paddr)
            print("%-8s 0x%08X 0x%06X   0x%06X   %s%s"
                  % (r[0] if r else "?非法", paddr, len(blob), memsz,
                     "X" if flags & 1 else "-", "W" if flags & 2 else "-"))
    return bytes(img), problems


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("-o", "--out", required=True, help="输出 .bin（原始字节）")
    ap.add_argument("--pad-to", default=None, help="把镜像补齐到指定字节数（如 0x100000）")
    ap.add_argument("--hex", default=None, help="另存字级 hex 文本（每行一个 32 位字）")
    ap.add_argument("--sym", default=None, help="另存符号表 addr,name")
    ap.add_argument("--report", action="store_true")
    memmap.add_arg(ap)
    a = ap.parse_args(argv)
    global REGIONS
    REGIONS = memmap.get(a.map).regions

    elf = Elf(a.elf)
    pad = int(a.pad_to, 0) if a.pad_to else None
    img, problems = build(elf, pad_to=pad, verbose=True)
    for p in problems:
        print("[FAIL] " + p)
    if problems:
        print("镜像未写出（%d 项不合格）" % len(problems))
        return 1
    open(a.out, "wb").write(img)
    print("写出 %s：%d 字节（entry=0x%08X）" % (a.out, len(img), elf.entry))
    if a.hex:
        words = [int.from_bytes(img[i:i + 4].ljust(4, b"\x00"), "little")
                 for i in range(0, len(img), 4)]
        with open(a.hex, "w", encoding="utf-8") as f:
            for w in words:
                f.write("%08X\n" % w)
        print("写出 %s：%d 个字" % (a.hex, len(words)))
    if a.sym:
        with open(a.sym, "w", encoding="utf-8") as f:
            for val, sz, typ, name in sorted(elf.syms, key=lambda t: t[0]):
                if typ in (0, 2) and val:      # NOTYPE / FUNC
                    f.write("%08X %s\n" % (val, name))
        print("写出 %s：%d 个符号" % (a.sym, len(elf.syms)))
    if a.report:
        print("入口 e_entry = 0x%08X（硬件复位向量应为 0x0，落位由 link.ld 保证）"
              % elf.entry)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
