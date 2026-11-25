#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""rv32emu.py —— TCRV32 的 L1 指令级模拟器（golden model）

定位：PC 上的"真机参照"。游戏内 CPU 的一切行为都必须与它逐条一致；
所有 C/汇编先在它上面跑通，再谈硬件。

显式建模的平台约束（与 ../RV32IM-ISA规范.md 一致）：
  * RV32IM 全量；无 C 扩展（指令必须 4 字节对齐，低 2 位必须是 0b11）
  * 小端；非对齐访问原生支持；但"跨 8 字节行的 4 字节访问"→ 故障
  * 16 MiB 物理 RAM（0x0000_0000 ~ 0x00FF_FFFF），按区域划分权限
  * MMIO 在 RAM 之外（0x4000_0000 起），越界/未映射 → 故障（不是静默读 0）
  * EBREAK = 停机；ECALL = 系统调用（a7 功能号）；FENCE/FENCE.I = NOP
  * 非法编码 = 故障（不做"当 NOP 执行"）

用法：
  python emu/rv32emu.py --hex tests/edge.hex --sp 0x003FF000 --dump-regs
  python emu/rv32emu.py --bin build/app.bin --entry 0 --trace --max 200000
  python emu/rv32emu.py --hex x.hex --dump-mem 0x3FF000:32 --json

退出码：0 = 正常停机；1 = 故障（含非法指令/越界）；2 = 用法错误。
"""

import argparse
import json
import sys

XLEN = 32
MASK32 = 0xFFFFFFFF
# ---- 内存地图：两套并存，由 --map 选择（默认 1 GiB） ----------------------
import os as _os
import sys as _sys
_sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))
import memmap as _memmap

MMIO_BASE = 0x4000_0000          # 外设区基址（两套地图都一样）
MMIO_SIZE = 0x0000_1000          # 4 KiB
MEM_SIZE = _memmap.get(_memmap.DEFAULT).mem_size
REGIONS = _memmap.get(_memmap.DEFAULT).regions
STACK_TOP = _memmap.get(_memmap.DEFAULT).stack_top


def apply_map(key):
    """切换内存地图：改本模块的 MEM_SIZE / REGIONS / STACK_TOP"""
    global MEM_SIZE, REGIONS, STACK_TOP
    m = _memmap.get(key)
    MEM_SIZE, REGIONS, STACK_TOP = m.mem_size, m.regions, m.stack_top

# MMIO 寄存器偏移
MMIO_CONSOLE = 0x00              # 写=输出低字节；读=输入键码（无则 -1）
MMIO_HALT    = 0x10              # 写=停机，值为退出码
MMIO_TIMER   = 0x20              # 读=毫秒（用已执行指令数/1000 近似）
MMIO_KEY     = 0x30              # 读=键码
MMIO_SCR_WIN = 0x200             # 屏幕设置窗口基址
MMIO_SCR_N   = 2                 # 支持的屏幕数
# 键盘窗口（MMIO 槽 2：0x400–0x5FF，见 works/总结参考/22-键盘与GPU方案.md §2.3）
#   硬件按 addr[11:9]==2 选槽、按 addr[4:2] 选寄存器 ⇒ 每 0x20 字节一组"有意别名"
#   +0x00 PEEK  读：bit[8]=按下位(1=按下 0=弹起), bit[7:0]=键码；只看不取
#   +0x04 STAT  读：bit0 = 事件队列非空
#   +0x08 POP   写任意值 = 弹出一个事件（对应 com_keyboard 的 Read next 单拍脉冲）
MMIO_KBD_WIN = 0x400
MMIO_KBD_SZ  = 0x200
# GPU 窗口（MMIO 槽 3：0x600–0x7FF，见 works/总结参考/22-键盘与GPU方案.md §4.3）
#   寄存器从窗口 +0x00 起，每 4 字节一个；CMD 是写触发，STAT/PIX/ACC 只读
MMIO_GPU_WIN = 0x600
MMIO_GPU_SZ  = 0x200
#   0x200 + 0x10*屏幕号 + 0x04*域    域 0 = 选项号，域 1 = 值
MMIO_SCR_OPT = 0x200             # 屏幕 0 选项号（兼容旧名）
MMIO_SCR_VAL = 0x204             # 屏幕 0 值（兼容旧名）

ABI = ["zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2",
       "s0", "s1", "a0", "a1", "a2", "a3", "a4", "a5",
       "a6", "a7", "s2", "s3", "s4", "s5", "s6", "s7",
       "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6"]


class Fault(Exception):
    def __init__(self, kind, addr=None, inst=None, detail=""):
        self.kind, self.addr, self.inst, self.detail = kind, addr, inst, detail
        super().__init__(kind)


def sext(v, bits):
    v &= (1 << bits) - 1
    return v - (1 << bits) if v >> (bits - 1) else v


def s32(v):
    v &= MASK32
    return v - (1 << 32) if v >> 31 else v


class Emu:
    def __init__(self, map_name=None, mem_size=MEM_SIZE, stdin_text="", echo=True, strict_row=False,
                 preload_sp=False, strict_regions=False, keys=None):
        if map_name:
            apply_map(map_name)
        if mem_size < STACK_TOP:
            raise ValueError(
                "mem_size=0x%08X 小于栈顶 0x%08X：内存地图已扩到 1 GiB，"
                "不要再用 16 MiB 覆写 mem_size" % (mem_size, STACK_TOP))
        self.mem = bytearray(mem_size)
        self.r = [0] * 32
        self.pc = 0
        # 复位后寄存器全 0 —— 与硬件一致（寄存器堆是 RAM，没有"预置栈顶"的能力）。
        # 栈指针由 crt0 自己设置。preload_sp=True 只在复现旧行为时用。
        if preload_sp:
            self.r[2] = STACK_TOP
        # 硬件没有分段写保护（只能整块不装写口），所以默认不强制"区域只读"，
        # 否则会出现"游戏里能跑、模拟器报错"的假差异。strict_regions=True 恢复旧行为。
        self.strict_regions = strict_regions
        # 屏幕设置（MMIO 0x200 / 0x204）
        self.scr_opt = [0] * MMIO_SCR_N   # 每屏一个选项号寄存器
        self.scr_val = [0] * MMIO_SCR_N
        self.scr_log = []          # [(屏幕号, 选项, 值), ...] 每次"写入设置"记一笔
        self.out_log = []          # 每次写 OUT 记一笔（32 位整字）
        # 键盘：事件队列 + 脚本化注入（keys = [(tick, down, code), ...]）
        self.kbd_q = []            # [(down, code), ...]，队头 = 下一条待取事件
        # 只按 tick 排序（稳定排序，同 tick 的多条保持给定顺序）
        self.key_script = sorted([(int(t), int(d) & 1, int(c) & 0xFF)
                                  for (t, d, c) in (keys or ())],
                                 key=lambda item: item[0])
        self.key_pos = 0
        self.kbd_popped = 0        # 累计弹出条数（调试用）
        # GPU（MMIO 槽 3）：一套参数寄存器 + 一条二维命令的状态机
        #   语义与硬件一致：CMD 写触发、每拍推进 BPT 个字节、busy/done/err、PIX 计数
        self.gpu_bpt = 8
        self.gpu_reg = [0] * 12    # 0 VER / 1 CMD / 2 DST / 3 SRC / 4 LEN / 5 ROWS
                                   # 6 STRIDE / 7 ARG0 / 8 ARG1 / 9 STAT / 10 PIX / 11 ACC
        self.gpu_reg[0] = 0x0002_0000
        self.gpu_busy = False
        self.gpu_done = False
        self.gpu_err = False
        self.gpu_col = 0
        self.gpu_row = 0
        self.gpu_pix = 0
        self.gpu_acc = 0
        self.gpu_op = 0
        self.gpu_cmds = 0          # 累计命令数（调试用）
        self.halted = False
        self.exit_code = 0
        self.halt_pc = None
        self.instret = 0
        self.out = []
        self.stdin = list(stdin_text)
        self.echo = echo
        self.trace = False
        self.trace_lines = []
        self.steps_limit = 50_000_000
        self.stop_at = set()          # 断点：pc 命中即停机（用于检查中间状态）
        # 硬件用内存端口"按字节拼接"，任意偏移的 1/2/4 字节访问都成立（见 ISA 规范 §3.4）。
        # strict_row=True 只在复现旧的 8-bank 自建存储体方案时才需要。
        self.strict_row = strict_row

    # ---------------- 内存与区域权限 ----------------
    def region_of(self, addr):
        for name, base, size, ex, wr in REGIONS:
            if base <= addr < base + size:
                return name, ex, wr
        return None

    def _check_data(self, addr, size, write):
        end = addr + size - 1
        if end >= MEM_SIZE and addr < MMIO_BASE:
            raise Fault("数据地址越界", addr)
        if addr < MMIO_BASE:
            for probe in (addr, end):
                reg = self.region_of(probe)
                if reg is None:
                    raise Fault("未映射地址", probe)
                if self.strict_regions and write and not reg[2]:
                    raise Fault("区域只读(%s)" % reg[0], probe)
        if self.strict_row and size > 1 and (addr & 7) + size > 8:
            raise Fault("跨 8 字节行访问（strict_row 才启用）", addr)

    def read(self, addr, size):
        addr &= MASK32
        if addr >= MMIO_BASE:
            return self.mmio_read(addr, size)
        self._check_data(addr, size, False)
        return int.from_bytes(self.mem[addr:addr + size], "little")

    def write(self, addr, size, value):
        addr &= MASK32
        if addr >= MMIO_BASE:
            self.mmio_write(addr, size, value)
            return
        self._check_data(addr, size, True)
        self.mem[addr:addr + size] = (value & ((1 << (8 * size)) - 1)).to_bytes(size, "little")

    def fetch(self, addr):
        if not (REGIONS[0][1] <= addr < REGIONS[0][1] + REGIONS[0][2]):
            raise Fault("取指地址不在 IMEM", addr)
        if addr & 3:
            raise Fault("取指地址未 4 字节对齐", addr)
        return int.from_bytes(self.mem[addr:addr + 4], "little")

    # ---------------- MMIO ----------------
    def mmio_read(self, addr, size):
        off = addr - MMIO_BASE
        if off == MMIO_CONSOLE:
            if self.stdin:
                return ord(self.stdin.pop(0))
            return MASK32                     # -1：无输入
        if off == MMIO_HALT:
            return 0
        if off == MMIO_TIMER:
            return (self.instret // 1000) & MASK32
        if off == MMIO_KEY:
            return ord(self.stdin.pop(0)) if self.stdin else MASK32
        if MMIO_SCR_WIN <= off < MMIO_SCR_WIN + 0x10 * MMIO_SCR_N:
            d = off - MMIO_SCR_WIN
            sid, field = d >> 4, (d >> 2) & 3
            if field == 0:
                return self.scr_opt[sid] & MASK32
            if field == 1:
                return self.scr_val[sid] & MASK32
            raise Fault("未知屏幕域", addr)
        if MMIO_KBD_WIN <= off < MMIO_KBD_WIN + MMIO_KBD_SZ:
            k = (off & 0x1C) >> 2        # addr[4:2]：与硬件一致的别名粒度
            if k == 0:
                return self.kbd_peek()   # PEEK：只看不取
            if k == 1:
                return 1 if self.kbd_q else 0
            if k == 2:
                # EVENT：同一拍取走并弹出（游戏自己的 keyboard 指令就是这个语义）
                v = self.kbd_peek()
                if self.kbd_q:
                    self.kbd_q.pop(0)
                    self.kbd_popped += 1
                return v
            return 0
        if MMIO_GPU_WIN <= off < MMIO_GPU_WIN + MMIO_GPU_SZ:
            r = (off - MMIO_GPU_WIN) >> 2
            if r == 9:                                 # STAT
                return (1 if self.gpu_busy else 0) | (2 if self.gpu_done else 0) | \
                       (4 if self.gpu_err else 0)
            if r == 10:
                return self.gpu_pix & MASK32
            if r == 11:
                return self.gpu_acc & MASK32
            if 0 <= r < len(self.gpu_reg):
                return self.gpu_reg[r] & MASK32
            return 0
        raise Fault("未知 MMIO 读", addr)

    def mmio_write(self, addr, size, value):
        off = addr - MMIO_BASE
        if off == MMIO_CONSOLE:
            self.out_log.append(value & MASK32)      # 记录完整 32 位（硬件 OUT 就是 32 位）
            self.putch(value & 0xFF)
            return
        if off == MMIO_HALT:
            self.exit_code = value & 0xFF
            self.halt()
            return
        if MMIO_SCR_WIN <= off < MMIO_SCR_WIN + 0x10 * MMIO_SCR_N:
            d = off - MMIO_SCR_WIN
            sid, field = d >> 4, (d >> 2) & 3
            if field == 0:
                self.scr_opt[sid] = value & 7
                return
            if field == 1:
                self.scr_val[sid] = value & MASK32
                self.scr_log.append((sid, self.scr_opt[sid], self.scr_val[sid]))
                return
            raise Fault("未知屏幕域", addr)
        if off in (MMIO_TIMER, MMIO_KEY):
            return
        if MMIO_KBD_WIN <= off < MMIO_KBD_WIN + MMIO_KBD_SZ:
            k = (off & 0x1C) >> 2
            if k == 2 and self.kbd_q:        # 写 POP = 弹出一条；空队列什么也不做
                self.kbd_q.pop(0)
                self.kbd_popped += 1
            return
        if MMIO_GPU_WIN <= off < MMIO_GPU_WIN + MMIO_GPU_SZ:
            r = (off - MMIO_GPU_WIN) >> 2
            if 0 <= r < 9:
                self.gpu_reg[r] = value & MASK32
                if r == 1:                   # CMD：写触发
                    self._gpu_start(value & 0xFF)
            return                       # STAT/PIX/ACC 只读：写入忽略
        raise Fault("未知 MMIO 写", addr)

    def putch(self, ch):
        self.out.append(chr(ch))
        if self.echo:
            sys.stdout.write(chr(ch))
            sys.stdout.flush()

    def halt(self):
        if not self.halted:
            self.halted = True
            self.halt_pc = self.pc

    # ---------------- 系统调用 ----------------
    def syscall(self):
        a7 = self.r[17]
        if a7 in (1, 2):                         # 输出一个字符（2 是主用，1 是兼容别名）
            self.putch(self.r[10] & 0xFF)
        elif a7 == 3:                            # read() -> 键码 / -1
            self.r[10] = self.mmio_read(MMIO_BASE + MMIO_CONSOLE, 1)
        elif a7 == 4:                            # time_ms
            self.r[10] = (self.instret // 1000) & MASK32
        elif a7 == 5:                            # gpu stub
            self.r[10] = 0
        elif a7 == 60:                           # exit(code)
            self.exit_code = self.r[10] & 0x7F
            self.halt()
        else:
            raise Fault("未知系统调用 a7=%d" % a7)

    # ---------------- 键盘 ----------------
    def kbd_push(self, down, code):
        """投一条键盘事件（等价于玩家按/松一次键）。"""
        self.kbd_q.append((int(down) & 1, int(code) & 0xFF))

    # ---------------- GPU（内存→内存二维引擎，MMIO 槽 3） ----------------
    def _gpu_start(self, op):
        """CMD 写触发：锁存 op 并启动一次两维循环（硬件语义，见 22 号 §4.4）。"""
        if self.gpu_busy:                 # 参数寄存器只有一套：忙时再提交 = 软件错误
            self.gpu_err = True
            return
        if not (0x01 <= op <= 0x0B):
            self.gpu_err = True
            self.gpu_done = True
            self.gpu_busy = False
            return
        self.gpu_op = op
        self.gpu_col = 0
        self.gpu_row = 0
        self.gpu_pix = 0
        self.gpu_acc = 0
        self.gpu_err = False
        self.gpu_done = False
        self.gpu_cmds += 1
        self.gpu_busy = True
        if self.gpu_reg[4] == 0 or self.gpu_reg[5] == 0:   # LEN/ROWS 为 0：空命令
            self.gpu_busy = False
            self.gpu_done = True

    def _gpu_tick(self):
        """每拍推进：处理 BPT 个连续字节，行满换行，行数满则完成。"""
        if not self.gpu_busy:
            return
        dst, src = self.gpu_reg[2], self.gpu_reg[3]
        ln, rows, stride = self.gpu_reg[4], self.gpu_reg[5], self.gpu_reg[6]
        arg0 = self.gpu_reg[7] & 0xFF
        args = self.gpu_reg[8]

        base_d = (dst + self.gpu_row * stride) & MASK32
        base_s = (src + self.gpu_row * stride) & MASK32
        op = self.gpu_op
        for j in range(self.gpu_bpt):
            if self.gpu_col + j >= ln:
                break
            sval = self.mem[(base_s + self.gpu_col + j) & (len(self.mem) - 1)] \
                if base_s + self.gpu_col + j < len(self.mem) else 0
            if op == 0x01:
                val = arg0
            elif op == 0x0B:                       # SUM：只累加，不写
                self.gpu_acc = (self.gpu_acc + sval) & MASK32
                self.gpu_pix += 1
                continue
            elif op == 0x02:
                val = sval
            elif op == 0x03:
                val = (sval + arg0) & 0xFF
            elif op == 0x04:
                val = (sval - arg0) & 0xFF
            elif op == 0x05:
                val = min(sval, arg0)
            elif op == 0x06:
                val = max(sval, arg0)
            elif op == 0x07:
                dval = self.mem[(base_d + self.gpu_col + j) & (len(self.mem) - 1)]
                val = (sval + dval) >> 1
            elif op == 0x08:
                val = sval & arg0
            elif op == 0x09:
                val = sval | arg0
            elif op == 0x0A:
                val = sval ^ arg0
            else:
                val = sval
            _ = args
            addr = (base_d + self.gpu_col + j) & MASK32
            if addr < len(self.mem):
                self.mem[addr] = val & 0xFF
            self.gpu_pix += 1

        self.gpu_col += self.gpu_bpt
        if self.gpu_col >= ln:
            self.gpu_col = 0
            self.gpu_row += 1
            if self.gpu_row >= rows:
                self.gpu_busy = False
                self.gpu_done = True

    def _kbd_inject(self):
        """按 tick 脚本注入事件；instret 就是 tick 数（1 指令 = 1 tick）。"""
        while self.key_pos < len(self.key_script) and \
                self.key_script[self.key_pos][0] <= self.instret:
            _, down, code = self.key_script[self.key_pos]
            self.kbd_q.append((down, code))
            self.key_pos += 1

    def kbd_peek(self):
        """队头事件：没有则返回 0（硬件 PEEK 高位自动零扩展）。"""
        if not self.kbd_q:
            return 0
        down, code = self.kbd_q[0]
        return ((down & 1) << 8) | (code & 0xFF)

    # ---------------- 单步 ----------------
    def step(self):
        self._kbd_inject()
        self._gpu_tick()
        pc = self.pc
        inst = self.fetch(pc)
        op = inst & 0x7F
        rd = (inst >> 7) & 31
        f3 = (inst >> 12) & 7
        rs1 = (inst >> 15) & 31
        rs2 = (inst >> 20) & 31
        f7 = inst >> 25
        r = self.r
        r[0] = 0
        npc = (pc + 4) & MASK32

        if op == 0x33:                                   # OP
            a, b = r[rs1], r[rs2]
            if f7 == 0x00:
                val = {0: (a + b), 1: (a << (b & 31)), 2: int(s32(a) < s32(b)),
                       3: int(a < b), 4: (a ^ b), 5: (a >> (b & 31)), 6: (a | b),
                       7: (a & b)}.get(f3)
            elif f7 == 0x20 and f3 == 0:
                val = a - b
            elif f7 == 0x20 and f3 == 5:
                val = s32(a) >> (b & 31)
            elif f7 == 0x01:
                val = self._muldiv(f3, a, b)
            else:
                raise Fault("非法 OP 编码", inst=inst)
            if val is None:
                raise Fault("非法 OP fun3", inst=inst)
            r[rd] = val & MASK32

        elif op == 0x13:                                 # OP-IMM
            a = r[rs1]
            imm = sext(inst >> 20, 12)
            if f3 in (0, 2, 3, 4, 6, 7):
                val = {0: a + imm, 2: int(s32(a) < imm), 3: int(a < (imm & MASK32)),
                       4: a ^ imm, 6: a | imm, 7: a & imm}[f3]
            elif f3 == 1:
                if f7 != 0x00:
                    raise Fault("非法 SLLI", inst=inst)
                val = a << (rs2 & 31)
            elif f3 == 5:
                sh = rs2 & 31
                if f7 == 0x00:
                    val = a >> sh
                elif f7 == 0x20:
                    val = s32(a) >> sh
                else:
                    raise Fault("非法 SRLI/SRAI", inst=inst)
            else:
                raise Fault("非法 OP-IMM", inst=inst)
            r[rd] = val & MASK32

        elif op == 0x03:                                 # LOAD
            ea = (r[rs1] + sext(inst >> 20, 12)) & MASK32
            if f3 == 0:
                r[rd] = sext(self.read(ea, 1), 8) & MASK32
            elif f3 == 1:
                r[rd] = sext(self.read(ea, 2), 16) & MASK32
            elif f3 == 2:
                r[rd] = self.read(ea, 4)
            elif f3 == 4:
                r[rd] = self.read(ea, 1)
            elif f3 == 5:
                r[rd] = self.read(ea, 2)
            else:
                raise Fault("非法 LOAD funct3", inst=inst)

        elif op == 0x23:                                 # STORE
            ea = (r[rs1] + sext((((inst >> 25) & 0x7F) << 5) | ((inst >> 7) & 0x1F), 12)) & MASK32
            if f3 == 0:
                self.write(ea, 1, r[rs2])
            elif f3 == 1:
                self.write(ea, 2, r[rs2])
            elif f3 == 2:
                self.write(ea, 4, r[rs2])
            else:
                raise Fault("非法 STORE funct3", inst=inst)

        elif op == 0x63:                                 # BRANCH
            a, b = r[rs1], r[rs2]
            off = sext((((inst >> 31) & 1) << 12) | (((inst >> 7) & 1) << 11) |
                       (((inst >> 25) & 0x3F) << 5) | (((inst >> 8) & 0xF) << 1), 13)
            taken = {0: a == b, 1: a != b, 4: s32(a) < s32(b), 5: s32(a) >= s32(b),
                     6: a < b, 7: a >= b}.get(f3)
            if taken is None:
                raise Fault("非法 BRANCH funct3", inst=inst)
            if taken:
                npc = (pc + off) & MASK32
                if npc & 3:
                    raise Fault("跳转目标未 4 字节对齐（IALIGN=32）", npc)

        elif op == 0x6F:                                 # JAL
            off = sext((((inst >> 31) & 1) << 20) | (((inst >> 12) & 0xFF) << 12) |
                       (((inst >> 20) & 1) << 11) | (((inst >> 21) & 0x3FF) << 1), 21)
            r[rd] = npc
            npc = (pc + off) & MASK32
            if npc & 3:
                raise Fault("跳转目标未 4 字节对齐（IALIGN=32）", npc)

        elif op == 0x67:                                 # JALR
            if f3 != 0:
                raise Fault("非法 JALR funct3", inst=inst)
            t = (r[rs1] + sext(inst >> 20, 12)) & MASK32 & ~1
            if t & 3:
                # 规范只清 bit0；IALIGN=32 还要求 bit1=0，否则是"指令地址未对齐"
                raise Fault("JALR 目标未 4 字节对齐（IALIGN=32）", t)
            r[rd] = npc
            npc = t

        elif op == 0x37:                                 # LUI
            r[rd] = inst & 0xFFFFF000

        elif op == 0x17:                                 # AUIPC
            r[rd] = (pc + (inst & 0xFFFFF000)) & MASK32

        elif op == 0x0F:                                 # MISC-MEM：FENCE / FENCE.I
            if f3 not in (0, 1):
                raise Fault("非法 MISC-MEM funct3", inst=inst)

        elif op == 0x73:                                 # SYSTEM
            if inst == 0x00000073:
                self.syscall()
            elif inst == 0x00100073:
                self.exit_code = 0
                self.halt()
            else:
                raise Fault("非法 SYSTEM 编码", inst=inst)

        else:
            raise Fault("非法 opcode", inst=inst)

        if self.trace:
            self.trace_lines.append("  %08X: %08X  rd=%02d rs1=%02d rs2=%02d"
                                    % (pc, inst, rd, rs1, rs2))
        r[0] = 0
        if not self.halted:            # 停机后 pc 停在触发停机的那条指令上
            self.pc = npc
        self.instret += 1

    def _muldiv(self, f3, a, b):
        sa, sb = s32(a), s32(b)
        if f3 == 0:
            return (a * b) & MASK32
        if f3 == 1:
            return ((sa * sb) >> 32) & MASK32
        if f3 == 2:
            return ((sa * b) >> 32) & MASK32
        if f3 == 3:
            return ((a * b) >> 32) & MASK32
        if f3 == 4:                                   # DIV
            if sb == 0:
                return MASK32
            if sa == -(1 << 31) and sb == -1:
                return a & MASK32
            q = abs(sa) // abs(sb)
            if (sa < 0) != (sb < 0):
                q = -q
            return q & MASK32
        if f3 == 5:                                   # DIVU
            return MASK32 if b == 0 else (a // b) & MASK32
        if f3 == 6:                                   # REM
            if sb == 0:
                return a & MASK32
            if sa == -(1 << 31) and sb == -1:
                return 0
            q = abs(sa) // abs(sb)
            if (sa < 0) != (sb < 0):
                q = -q
            return (sa - q * sb) & MASK32
        if f3 == 7:                                   # REMU
            return (a & MASK32) if b == 0 else (a % b) & MASK32
        raise Fault("非法 MUL/DIV funct3")

    # ---------------- 运行 ----------------
    def run(self, max_steps=None):
        limit = min(max_steps or self.steps_limit, self.steps_limit)
        while not self.halted and self.instret < limit:
            if self.pc in self.stop_at:
                self.halt()
                break
            try:
                self.step()
            except Fault as f:
                f.pc = self.pc
                raise
        return self.halted

    # ---------------- 快照 ----------------
    def dump_regs(self):
        lines = ["PC=0x%08X  instret=%d  halted=%s exit=%d"
                 % (self.pc, self.instret, self.halted, self.exit_code)]
        for i in range(0, 32, 2):
            parts = []
            for j in (i, i + 1):
                if j < 32:
                    parts.append("%-4s=0x%08X(%11d)" % (ABI[j], self.r[j], s32(self.r[j])))
            lines.append("  " + "  ".join(parts))
        return "\n".join(lines)

    def dump_mem(self, base, length):
        out = []
        for i in range(0, length, 16):
            chunk = self.mem[base + i:base + i + min(16, length - i)]
            out.append("  %08X: %s" % (base + i, " ".join("%02X" % b for b in chunk)))
        return "\n".join(out)

    def load_hex(self, path, base=0):
        data = bytearray()
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                s = line.strip().split("//")[0].split("#")[0].strip()
                if not s:
                    continue
                data += int(s, 16).to_bytes(4, "little")
        self.mem[base:base + len(data)] = data
        return len(data)

    def load_bin(self, path, base=0):
        data = open(path, "rb").read()
        self.mem[base:base + len(data)] = data
        return len(data)


def parse_key_script(text):
    """把 "1000:01,1100:01:0" 解析成 [(tick, down, code), ...]

    每项 = tick:键码[:按下位]；键码按十六进制；按下位缺省 1（按下），0 = 弹起。
    为空则返回 []。给测试与 L1 对拍用：tick 就是已执行指令数。
    """
    out = []
    for item in (text or "").split(","):
        item = item.strip()
        if not item:
            continue
        f = item.split(":")
        if len(f) < 2:
            raise ValueError("--keys 项格式应为 tick:键码[:按下位]：%r" % item)
        out.append((int(f[0], 0), int(f[2], 0) if len(f) > 2 else 1, int(f[1], 16)))
    return out


def main(argv):
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("--hex", help="十六进制文本镜像（每行一个 32 位字）")
    ap.add_argument("--bin", help="原始二进制镜像")
    ap.add_argument("--base", default="0", help="镜像装入地址（默认 0）")
    ap.add_argument("--entry", default=None, help="入口地址（默认 = base）")
    ap.add_argument("--sp", default=None, help="初始 sp（默认 DMEM 顶端）")
    ap.add_argument("--max", default="2000000", help="最大指令数")
    ap.add_argument("--trace", action="store_true")
    ap.add_argument("--dump-regs", action="store_true")
    ap.add_argument("--dump-mem", default=None, help="如 0x3FF000:32")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--stdin", default="")
    ap.add_argument("--quiet", action="store_true", help="不回显 putchar")
    ap.add_argument("--strict-row", action="store_true",
                    help="启用旧的跨 8 字节行故障（仅复现 8-bank 存储体时用）")
    ap.add_argument("--strict-regions", action="store_true",
                    help="强制区域写保护（默认关闭：硬件做不到段保护）")
    _memmap.add_arg(ap)
    ap.add_argument("--keys", default="",
                    help="键盘事件脚本，如 \"1000:01,1100:01:0\"（tick:键码[:按下位]）")
    a = ap.parse_args(argv)
    apply_map(getattr(a, 'map', None))
    if not a.hex and not a.bin:
        ap.print_help()
        return 2
    e = Emu(stdin_text=a.stdin, echo=not a.quiet, strict_row=a.strict_row,
            strict_regions=a.strict_regions, keys=parse_key_script(a.keys))
    e.trace = a.trace
    base = int(a.base, 0)
    n = e.load_hex(a.hex, base) if a.hex else e.load_bin(a.bin, base)
    e.pc = int(a.entry, 0) if a.entry else base
    if a.sp:
        e.r[2] = int(a.sp, 0)
    sys.stderr.write("; 载入 %d 字节，entry=0x%08X sp=0x%08X\n" % (n, e.pc, e.r[2]))
    rc = 0
    try:
        e.run(int(a.max, 0))
        if not e.halted:
            sys.stderr.write("; 未停机（达到指令上限）\n")
            rc = 1
    except Fault as f:
        sys.stderr.write("; 故障: %s @ PC=0x%08X" % (f.kind, f.pc))
        if f.addr is not None:
            sys.stderr.write(" addr=0x%08X" % f.addr)
        if f.inst is not None:
            sys.stderr.write(" inst=0x%08X" % f.inst)
        sys.stderr.write("\n")
        rc = 1
    if e.trace:
        print("\n".join(e.trace_lines[-200:]))
    if a.dump_regs or rc:
        print(e.dump_regs())
    if a.dump_mem:
        spec, length = a.dump_mem.split(":")
        print(e.dump_mem(int(spec, 0), int(length, 0)))
    if a.json:
        print(json.dumps(dict(pc=e.pc, halted=e.halted, exit=e.exit_code,
                              instret=e.instret,
                              regs=e.r,
                              stdout="".join(e.out))))
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
