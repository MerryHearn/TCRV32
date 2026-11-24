#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""rv32asm.py —— 极简 RV32IM 汇编器 / 反汇编器 / 边界用例生成器

用途: 在没有 RISC-V 工具链的机器上直接产出可喂给自制 CPU 的机器码,
      并把最阴的边界用例固化成回归向量。

用法:
  python rv32asm.py --selftest            编码自检(与标准编码对拍) + 反汇编回环
  python rv32asm.py --edge               生成边界用例机器码(带反汇编注释)
  python rv32asm.py -o demo.hex demo.s   汇编 .s -> 每行 8 位十六进制
  python rv32asm.py --dis 0xFFB00093     反汇编单个字
  python rv32asm.py --list               打印支持的助记符

地址模型: 绝对地址, 基址 0(--base 可改)。分支/跳转按 PC 相对编码。
不支持的: 压缩指令(故意的)、浮点、重定位、链接松弛。
参考: 本目录 riscv-spec.pdf 第 2 章(RV32I) / 第 9 章(M) / 第 11 章(Zicsr)。
"""

import sys
import re

# ---------------------------------------------------------------- 寄存器
_ABI = ["zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2",
        "s0", "s1", "a0", "a1", "a2", "a3", "a4", "a5",
        "a6", "a7", "s2", "s3", "s4", "s5", "s6", "s7",
        "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6"]
REGS = {}
for _i in range(32):
    REGS["x%d" % _i] = _i
    REGS[_ABI[_i]] = _i
REGS["fp"] = 8


def reg(tok):
    t = tok.strip()
    if t not in REGS:
        raise SyntaxError("未知寄存器 %r" % t)
    return REGS[t]


# ---------------------------------------------------------------- 编码
def enc_r(op, f3, f7, rd, rs1, rs2):
    return ((f7 & 0x7F) << 25) | ((rs2 & 31) << 20) | ((rs1 & 31) << 15) | \
           ((f3 & 7) << 12) | ((rd & 31) << 7) | (op & 0x7F)


def enc_i(op, f3, rd, rs1, imm):
    if not -2048 <= imm <= 2047:
        raise ValueError("I 立即数越界 %d" % imm)
    return ((imm & 0xFFF) << 20) | ((rs1 & 31) << 15) | ((f3 & 7) << 12) | \
           ((rd & 31) << 7) | (op & 0x7F)


def enc_s(op, f3, rs1, rs2, imm):
    if not -2048 <= imm <= 2047:
        raise ValueError("S 立即数越界 %d" % imm)
    u = imm & 0xFFF
    return (((u >> 5) & 0x7F) << 25) | ((rs2 & 31) << 20) | ((rs1 & 31) << 15) | \
           ((f3 & 7) << 12) | ((u & 0x1F) << 7) | (op & 0x7F)


def enc_b(op, f3, rs1, rs2, imm):
    if imm & 1:
        raise ValueError("分支偏移必须是偶数 %d" % imm)
    if not -4096 <= imm <= 4094:
        raise ValueError("B 立即数越界 %d" % imm)
    u = imm & 0x1FFF
    return (((u >> 12) & 1) << 31) | (((u >> 5) & 0x3F) << 25) | \
           ((rs2 & 31) << 20) | ((rs1 & 31) << 15) | ((f3 & 7) << 12) | \
           (((u >> 1) & 0xF) << 8) | (((u >> 11) & 1) << 7) | (op & 0x7F)


def enc_u(op, rd, imm20):
    return ((imm20 & 0xFFFFF) << 12) | ((rd & 31) << 7) | (op & 0x7F)


def enc_j(op, rd, imm):
    if imm & 1:
        raise ValueError("J 偏移必须是偶数 %d" % imm)
    if not -(1 << 20) <= imm < (1 << 20):
        raise ValueError("J 立即数越界 %d" % imm)
    u = imm & 0x1FFFFF
    return (((u >> 20) & 1) << 31) | (((u >> 1) & 0x3FF) << 21) | \
           (((u >> 11) & 1) << 20) | (((u >> 12) & 0xFF) << 12) | \
           ((rd & 31) << 7) | (op & 0x7F)


# ---------------------------------------------------------------- 指令表
R_F7 = {
    0: [(0x00, "add"), (0x20, "sub"), (0x01, "mul")],
    1: [(0x00, "sll"), (0x01, "mulh")],
    2: [(0x00, "slt"), (0x01, "mulhsu")],
    3: [(0x00, "sltu"), (0x01, "mulhu")],
    4: [(0x00, "xor"), (0x01, "div")],
    5: [(0x00, "srl"), (0x20, "sra"), (0x01, "divu")],
    6: [(0x00, "or"), (0x01, "rem")],
    7: [(0x00, "and"), (0x01, "remu")],
}
R_NAME = {n: (f3, f7) for f3, lst in R_F7.items() for f7, n in lst}

I_ALU = {"addi": 0, "slti": 2, "sltiu": 3, "xori": 4, "ori": 6, "andi": 7}
I_SHIFT = {"slli": (1, 0x00), "srli": (5, 0x00), "srai": (5, 0x20)}
I_ALU_F3 = {v: k for k, v in I_ALU.items()}

LOAD_F3 = {"lb": 0, "lh": 1, "lw": 2, "lbu": 4, "lhu": 5}
LOAD_F3_NAME = {v: k for k, v in LOAD_F3.items()}
STORE_F3 = {"sb": 0, "sh": 1, "sw": 2}
STORE_F3_NAME = {v: k for k, v in STORE_F3.items()}
BR_F3 = {"beq": 0, "bne": 1, "blt": 4, "bge": 5, "bltu": 6, "bgeu": 7}
BR_F3_NAME = {v: k for k, v in BR_F3.items()}

CSR_F3 = {"csrrw": 1, "csrrs": 2, "csrrc": 3,
          "csrrwi": 5, "csrrsi": 6, "csrrci": 7}
CSR_F3_NAME = {v: k for k, v in CSR_F3.items()}
CSR_ADDR = {"mstatus": 0x300, "misa": 0x301, "mie": 0x304, "mtvec": 0x305,
            "mscratch": 0x340, "mepc": 0x341, "mcause": 0x342, "mtval": 0x343,
            "mip": 0x344, "cycle": 0xC00, "time": 0xC01, "instret": 0xC02}
CSR_ADDR_NAME = {v: k for k, v in CSR_ADDR.items()}


# ---------------------------------------------------------------- 解析
def split_ops(s):
    out, depth, cur = [], 0, ""
    for ch in s:
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


MEMREF = re.compile(r"^(.+?)\s*\(\s*([A-Za-z][\w.$]*)\s*\)$")


def num(tok, labels):
    t = tok.strip()
    if t in labels:
        return labels[t]
    if t.startswith("'") and t.endswith("'") and len(t) >= 3:
        return ord(t[1:-1])
    m = re.match(r"^([A-Za-z_.$][\w.$]*)\s*([+-])\s*(.+)$", t)
    if m and m.group(1) in labels:
        d = int(m.group(3), 0)
        return labels[m.group(1)] + (d if m.group(2) == "+" else -d)
    v = int(t, 0)
    return v - (1 << 32) if v >= (1 << 31) else v


def mem(tok, labels):
    m = MEMREF.match(tok.strip())
    if not m:
        raise SyntaxError("期望 offset(reg): %r" % tok)
    return num(m.group(1), labels), reg(m.group(2))


def expand(mn, ops):
    """伪指令展开 -> [(mnemonic, ops), ...]"""
    if mn == "nop":
        return [("addi", ["x0", "x0", "0"])]
    if mn == "mv":
        return [("addi", [ops[0], ops[1], "0"])]
    if mn == "not":
        return [("xori", [ops[0], ops[1], "-1"])]
    if mn == "neg":
        return [("sub", [ops[0], "x0", ops[1]])]
    if mn == "seqz":
        return [("sltiu", [ops[0], ops[1], "1"])]
    if mn == "snez":
        return [("sltu", [ops[0], "x0", ops[1]])]
    if mn == "j":
        return [("jal", ["x0", ops[0]])]
    if mn == "jr":
        return [("jalr", ["x0", "0(%s)" % ops[0]])]
    if mn == "ret":
        return [("jalr", ["x0", "0(ra)"])]
    if mn == "call":
        return [("jal", ["ra", ops[0]])]
    if mn in ("beqz", "bnez"):
        return [("beq" if mn == "beqz" else "bne", [ops[0], "x0", ops[1]])]
    if mn == "li":
        rd, val = ops[0], ops[1]
        if re.match(r"^[A-Za-z_.$]", val):
            return [("addi", [rd, "x0", val])]
        v = int(val, 0) & 0xFFFFFFFF          # 统一按 32 位无符号位型处理
        if v < 0x800 or v >= 0xFFFFF800:      # 能直接进 12 位有符号立即数
            sv = v if v < 0x800 else v - (1 << 32)
            return [("addi", [rd, "x0", str(sv)])]
        hi = ((v + 0x800) >> 12) & 0xFFFFF    # lui 的 imm[31:12], 含进位修正
        lo = v - (hi << 12)                   # 必在 [-2048, 2047]
        seq = [("lui", [rd, "0x%x" % hi])]
        if lo:
            seq.append(("addi", [rd, rd, str(lo)]))
        return seq
    if mn == "la":
        return [("_la", [ops[0], ops[1]])]
    if mn == "csrr":
        return [("csrrs", [ops[0], ops[1], "x0"])]
    if mn == "csrw":
        return [("csrrw", ["x0", ops[0], ops[1]])]
    return [(mn, ops)]


def size_of(seq):
    mn, ops = seq[0]
    if mn == "_la":
        return 8                      # lui + addi(总是两条, 安全)
    if mn == ".word":
        return 4 * len(ops)
    if mn == ".byte":
        return len(ops)
    return 4 * len(seq)


def emit(mn, ops, labels, pc, base):
    if mn == ".word":
        return [num(o, labels) & 0xFFFFFFFF for o in ops]
    if mn == ".byte":
        return [num(o, labels) & 0xFF for o in ops]
    if mn == "_la":
        rd = ops[0]
        addr = num(ops[1], labels) + base
        hi = ((addr + 0x800) >> 12) & 0xFFFFF
        lo = addr - (hi << 12)
        return [enc_u(0x37, reg(rd), hi), enc_i(0x13, 0, reg(rd), reg(rd), lo)]
    if mn in R_NAME:
        f3, f7 = R_NAME[mn]
        return [enc_r(0x33, f3, f7, reg(ops[0]), reg(ops[1]), reg(ops[2]))]
    if mn in I_SHIFT:
        f3, f7 = I_SHIFT[mn]
        sh = num(ops[2], labels)
        if not 0 <= sh <= 31:
            raise ValueError("移位量须 0..31: %d" % sh)
        imm = (f7 << 5) | sh
        return [((imm & 0xFFF) << 20) | (reg(ops[1]) << 15) | (f3 << 12) |
                (reg(ops[0]) << 7) | 0x13]
    if mn in I_ALU:
        return [enc_i(0x13, I_ALU[mn], reg(ops[0]), reg(ops[1]),
                      num(ops[2], labels))]
    if mn in LOAD_F3:
        off, rs1 = mem(ops[1], labels)
        return [enc_i(0x03, LOAD_F3[mn], reg(ops[0]), rs1, off)]
    if mn in STORE_F3:
        off, rs1 = mem(ops[1], labels)
        return [enc_s(0x23, STORE_F3[mn], rs1, reg(ops[0]), off)]
    if mn in BR_F3:
        tgt = num(ops[2], labels) + base
        return [enc_b(0x63, BR_F3[mn], reg(ops[0]), reg(ops[1]), tgt - pc)]
    if mn == "jal":
        tgt = num(ops[1], labels) + base
        return [enc_j(0x6F, reg(ops[0]), tgt - pc)]
    if mn == "jalr":
        off, rs1 = mem(ops[1], labels)
        return [enc_i(0x67, 0, reg(ops[0]), rs1, off)]
    if mn in ("lui", "auipc"):
        return [enc_u(0x37 if mn == "lui" else 0x17, reg(ops[0]),
                      num(ops[1], labels))]
    if mn in CSR_F3:
        csr = CSR_ADDR[ops[1]] if ops[1] in CSR_ADDR else int(ops[1], 0)
        src = int(ops[2], 0) if mn.endswith("i") else reg(ops[2])
        return [enc_i(0x73, CSR_F3[mn], reg(ops[0]), src, csr)]
    if mn == "ecall":
        return [0x00000073]
    if mn == "ebreak":
        return [0x00100073]
    if mn == "mret":
        return [0x30200073]
    if mn == "wfi":
        return [0x10500073]
    if mn == "fence":
        return [0x0FF0000F]
    if mn == "fence.i":
        return [0x0000100F]
    raise SyntaxError("未知助记符 %r" % mn)


def assemble(src, base=0):
    """-> ([(pc, [words])...], labels)"""
    items = []
    for ln, raw in enumerate(src.splitlines(), 1):
        s = raw.split("#")[0].split("//")[0].rstrip()
        if not s.strip():
            continue
        s = s.strip()
        while True:
            m = re.match(r"^([A-Za-z_.$][\w.$]*)\s*:\s*(.*)$", s)
            if not m:
                break
            items.append((ln, "label", m.group(1), None))
            s = m.group(2).strip()
            if not s:
                break
        if s:
            items.append((ln, "asm", None, s))

    labels, prog, pc = {}, [], base
    for ln, kind, name, body in items:
        if kind == "label":
            labels[name] = pc
            continue
        m = re.match(r"^(\S+)\s*(.*)$", body)
        mn, rest = m.group(1).lower(), m.group(2)
        seq = expand(mn, split_ops(rest) if rest.strip() else [])
        prog.append((ln, pc, body, seq))
        pc += size_of(seq)

    out, pc = [], base
    for ln, _pc0, body, seq in prog:
        if pc != _pc0:                  # 自检: 两遍地址必须一致
            raise SyntaxError("第 %d 行 [%s]: 地址漂移 0x%x != 0x%x"
                              % (ln, body, pc, _pc0))
        words = []
        for mn, ops in seq:
            try:
                ws = emit(mn, ops, labels, pc, base)
            except Exception as e:
                raise SyntaxError("第 %d 行 [%s]: %s" % (ln, body, e))
            words += ws
            pc += 4 * len(ws)          # 一条伪指令可能吐出多个字(_la/.word)
        out.append((_pc0, words))
    return out, labels


# ---------------------------------------------------------------- 反汇编
def sext(v, bits):
    v &= (1 << bits) - 1
    return v - (1 << bits) if v >> (bits - 1) else v


def disasm(w, pc=None):
    op, rd, f3 = w & 0x7F, (w >> 7) & 31, (w >> 12) & 7
    rs1, rs2, f7 = (w >> 15) & 31, (w >> 20) & 31, (w >> 25) & 0x7F
    if w == 0x00000073:
        return "ecall"
    if w == 0x00100073:
        return "ebreak"
    if w == 0x30200073:
        return "mret"
    if w == 0x10500073:
        return "wfi"
    if op == 0x0F:
        return {0: "fence", 1: "fence.i"}.get(f3, "misc-mem?")
    if op == 0x33:
        for f7v, n in R_F7.get(f3, []):
            if f7v == f7:
                return "%s x%d, x%d, x%d" % (n, rd, rs1, rs2)
        return "?R f3=%d f7=0x%02x" % (f3, f7)
    if op == 0x13:
        imm = (w >> 20) & 0xFFF
        if f3 in (1, 5):
            n = {1: {0: "slli"}, 5: {0: "srli", 0x20: "srai"}}[f3].get(imm >> 5)
            if n:
                return "%s x%d, x%d, %d" % (n, rd, rs1, imm & 31)
            return "?shift f3=%d f7=0x%02x" % (f3, imm >> 5)
        return "%s x%d, x%d, %d" % (I_ALU_F3[f3], rd, rs1, sext(imm, 12))
    if op == 0x03:
        n = LOAD_F3_NAME.get(f3)
        return ("%s x%d, %d(x%d)" % (n, rd, sext((w >> 20) & 0xFFF, 12), rs1)) \
            if n else "?load f3=%d" % f3
    if op == 0x23:
        n = STORE_F3_NAME.get(f3)
        imm = sext((((w >> 25) & 0x7F) << 5) | ((w >> 7) & 31), 12)
        return ("%s x%d, %d(x%d)" % (n, rs2, imm, rs1)) if n else "?store f3=%d" % f3
    if op == 0x63:
        n = BR_F3_NAME.get(f3)
        u = (((w >> 31) & 1) << 12) | (((w >> 7) & 1) << 11) | \
            (((w >> 25) & 0x3F) << 5) | (((w >> 8) & 0xF) << 1)
        off = sext(u, 13)
        t = "  -> 0x%08x" % ((pc + off) & 0xFFFFFFFF) if pc is not None else ""
        return ("%s x%d, x%d, %d%s" % (n, rs1, rs2, off, t)) if n else "?br f3=%d" % f3
    if op == 0x6F:
        u = (((w >> 31) & 1) << 20) | (((w >> 12) & 0xFF) << 12) | \
            (((w >> 20) & 1) << 11) | (((w >> 21) & 0x3FF) << 1)
        off = sext(u, 21)
        t = "  -> 0x%08x" % ((pc + off) & 0xFFFFFFFF) if pc is not None else ""
        return "jal x%d, %d%s" % (rd, off, t)
    if op == 0x67:
        return "jalr x%d, %d(x%d)" % (rd, sext((w >> 20) & 0xFFF, 12), rs1)
    if op in (0x37, 0x17):
        return "%s x%d, 0x%x" % ("lui" if op == 0x37 else "auipc", rd, w >> 12)
    if op == 0x73:
        n = CSR_F3_NAME.get(f3)
        if n:
            csr = (w >> 20) & 0xFFF
            cn = CSR_ADDR_NAME.get(csr, "0x%03x" % csr)
            return "%s x%d, %s, %s" % (n, rd, cn,
                                      str(rs1) if n.endswith("i") else "x%d" % rs1)
        return "?system f3=%d" % f3
    return "?0x%08x" % w


# ---------------------------------------------------------------- 自检
SELFTEST = [
    ("addi x1, x0, -5", 0xFFB00093),
    ("add x1, x2, x3", 0x003100B3),
    ("sub x5, x6, x7", 0x407302B3),
    ("xor x7, x8, x9", 0x009443B3),
    ("mul x1, x2, x3", 0x023100B3),
    ("divu x1, x2, x3", 0x023150B3),
    ("srai x1, x2, 3", 0x40315093),
    ("slli x5, x6, 31", 0x01F31293),
    ("lw x5, -8(x6)", 0xFF832283),
    ("lb x5, 3(x6)", 0x00330283),
    ("sw x5, 8(x6)", 0x00532423),
    ("sb x5, 7(x6)", 0x005303A3),
    ("beq x1, x2, -8", 0xFE208CE3),
    ("bne x1, x2, 8", 0x00209463),
    ("jal x1, 4", 0x004000EF),
    ("jalr x1, 0(x2)", 0x000100E7),
    ("lui x5, 0x12345", 0x123452B7),
    ("auipc x5, 0x12345", 0x12345297),
    ("ecall", 0x00000073),
    ("ebreak", 0x00100073),
    ("mret", 0x30200073),
    ("fence", 0x0FF0000F),
    ("fence.i", 0x0000100F),
    ("csrrw x1, mtvec, x2", 0x305110F3),
    ("csrrs x1, mcause, x0", 0x342020F3),
]

LI_TEST = ("li x5, 0x12345678", [0x123452B7, 0x67828293])


def selftest():
    ok = fail = 0
    print("== 1. 编码自检（对拍标准工具链编码） ==")
    for src, want in SELFTEST:
        words, _ = assemble(src)
        got = [w for _, ws in words for w in ws]
        good = got == [want]
        print("  %-22s -> %08X  %s" % (src, got[0], "OK" if good else
                                       "MISMATCH(期望 %08X)" % want))
        ok, fail = (ok + 1, fail) if good else (ok, fail + 1)
    src, want = LI_TEST
    words, _ = assemble(src)
    got = [w for _, ws in words for w in ws]
    good = got == want
    print("  %-22s -> %s  %s" % (src, " ".join("%08X" % w for w in got),
                                 "OK" if good else "MISMATCH(期望 %s)" %
                                 " ".join("%08X" % w for w in want)))
    ok, fail = (ok + 1, fail) if good else (ok, fail + 1)

    print("\n== 2. 反汇编回环（disasm -> 再汇编 -> 同一字） ==")
    for src, want in SELFTEST:
        txt = disasm(want)
        words, _ = assemble(txt)
        got = [w for _, ws in words for w in ws]
        good = got == [want]
        print("  %08X  %-22s -> %-24s %s" % (want, src, txt,
                                             "OK" if good else "FAIL"))
        ok, fail = (ok + 1, fail) if good else (ok, fail + 1)

    print("\n== 3. 标签与分支偏移 ==")
    src = """
    _start: addi x1, x0, 1
            beq  x1, x1, done
            addi x2, x0, 2
    done:   ebreak
    """
    words, labels = assemble(src)
    flat = [w for _, ws in words for w in ws]
    want = [0x00100093, 0x00108463, 0x00200113, 0x00100073]
    good = flat == want
    print("  beq 目标解析 -> %s  %s" % (" ".join("%08X" % w for w in flat),
                                        "OK" if good else "FAIL(期望 %s)" %
                                        " ".join("%08X" % w for w in want)))
    ok, fail = (ok + 1, fail) if good else (ok, fail + 1)

    print("\n结果: %d 通过 / %d 失败" % (ok, fail))
    return fail == 0


# ---------------------------------------------------------------- 边界用例
EDGE = """
# ===== RV32IM 边界用例：每条针对一类典型连线错误 =====
_start:
    # 1) x0 必须硬连零
    addi x0, x0, 5
    add  x0, x1, x2

    # 2) 有符号 / 无符号比较必须分开(x2=sp 保留, 不要占用)
    li   x1, -1
    li   x19, 1
    slt  x3, x1, x19
    sltu x4, x1, x19
    sltiu x5, x1, 1
    bltu x1, x19, fail
    blt  x1, x19, ok1
    j    fail
ok1:
    # 3) 算术右移 vs 逻辑右移；寄存器移位必须只取 rs2[4:0]
    li   x6, -16
    srai x7, x6, 2
    srli x8, x6, 2
    li   x1, 36
    sll  x9, x6, x1
    srai x10, x6, 1

    # 4) 字节/半字扩展
    li   x10, 0x8000FF80
    sw   x10, 0(sp)
    lb   x11, 3(sp)
    lbu  x12, 3(sp)
    lh   x13, 2(sp)
    lhu  x14, 2(sp)
    lb   x15, 0(sp)

    # 5) 子字写的字节通道（4 个偏移各测一次）
    sw   x0, 4(sp)
    li   x16, 0xAABBCCDD
    sb   x16, 4(sp)
    sb   x16, 7(sp)
    sh   x16, 6(sp)
    lw   x17, 4(sp)

    # 6) MULH 的四种符号组合
    li   x18, -3
    li   x19, 7
    mulh x20, x18, x19
    mulhu x21, x18, x19
    mulhsu x22, x18, x19

    # 7) 除法边界
    li   x23, -2147483648
    li   x24, -1
    div  x25, x23, x24
    rem  x26, x23, x24
    div  x27, x19, x0
    rem  x28, x19, x0

    # 8) JALR 必须清最低位
    la   x29, target
    addi x29, x29, 1
    jalr x30, 0(x29)
    j    fail
target:
    # 9) 立即数拼接与进位修正
    li   x31, 0x7FFFF800
    li   x5, 0x800
    li   x6, 0xFFFFF800

ok_end:
    ebreak
fail:
    ebreak
"""


def gen_edge(out=None):
    words, labels = assemble(EDGE)
    flat = [w for _, ws in words for w in ws]
    if out:
        with open(out, "w", encoding="utf-8") as f:
            f.write("".join("%08X\n" % (w & 0xFFFFFFFF) for w in flat))
        print("; 已写出 %s (%d 字 = %d 字节)" % (out, len(flat), 4 * len(flat)))
    print("== 标签 ==")
    for k, v in sorted(labels.items(), key=lambda kv: kv[1]):
        print("  %-8s @ 0x%08x" % (k, v))
    n = sum(len(ws) for _, ws in words)
    print("\n== 机器码（%d 字 = %d 字节）==" % (n, 4 * n))
    for pc0, ws in words:
        for w in ws:
            print("  %04X: %08X   %s" % (pc0, w, disasm(w, pc0)))
            pc0 += 4


def main(argv):
    base = int(argv[argv.index("--base") + 1], 0) if "--base" in argv else 0
    out = argv[argv.index("-o") + 1] if "-o" in argv else None
    if "--selftest" in argv:
        return 0 if selftest() else 1
    if "--edge" in argv:
        gen_edge(out)
        return 0
    if "--dis" in argv:
        w = int(argv[argv.index("--dis") + 1], 0)
        print("0x%08x  %s" % (w, disasm(w)))
        return 0
    if "--list" in argv:
        print("R    :", " ".join(sorted(R_NAME)))
        print("I ALU:", " ".join(sorted(I_ALU)))
        print("I 移位:", " ".join(sorted(I_SHIFT)))
        print("Load :", " ".join(sorted(LOAD_F3)))
        print("Store:", " ".join(sorted(STORE_F3)))
        print("Branch:", " ".join(sorted(BR_F3)))
        print("CSR  :", " ".join(sorted(CSR_F3)))
        print("其他 : jal jalr lui auipc ecall ebreak mret wfi fence fence.i")
        print("伪指令: nop mv not neg seqz snez j jr ret call beqz bnez li la csrr csrw")
        print("指示符: .word .byte")
        return 0
    src_path = out = None
    for i, a in enumerate(argv):
        if a == "-o":
            out = argv[i + 1]
        elif a.endswith((".s", ".S", ".asm")):
            src_path = a
    if not src_path:
        print(__doc__)
        return 2
    with open(src_path, encoding="utf-8") as f:
        src = f.read()
    words, labels = assemble(src, base)
    flat = [w for _, ws in words for w in ws]
    if out:
        with open(out, "w", encoding="utf-8") as f:
            f.write("".join("%08X\n" % (w & 0xFFFFFFFF) for w in flat))
    for k, v in sorted(labels.items(), key=lambda kv: kv[1]):
        print("; %-10s 0x%08x" % (k, v))
    print("; %d 字 -> %s" % (len(flat), out or "stdout"))
    if not out:
        for w in flat:
            print("%08X" % (w & 0xFFFFFFFF))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
