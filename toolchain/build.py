#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""build.py —— TCRV32 一键构建（自包含版）

把若干 C 源文件编译、链接、并转成可以直接装进游戏内存的二进制镜像。

用法
    python build.py <源文件> [更多源文件...] -o <输出.bin> [选项]

例子
    python build.py ../examples/hello.c -o ../build/hello.bin
    python build.py ../examples/hanoi.c -o ../build/hanoi.bin
    python build.py a.c b.c -o out.bin --elf out.elf --keep
    python build.py ../examples/hello.c -o ../build/hello.bin --map 16m
    python build.py my_gpu.c -o ../build/my_gpu.bin --gpu          # 只有 1 GiB 配置有 GPU

选项
    -o PATH     输出的 .bin 路径（必需）
    --elf PATH  同时保留 ELF 到指定路径（默认写到临时目录后删除）
    --pad N     把输出补齐到 N 字节（可写 0x100000 这样的十六进制）
    --map NAME  内存配置：16m（16 MiB）或 1g（1 GiB，默认）
    --gpu       额外编入 GPU 驱动 src/gpu.c；GPU 只在 1 GiB 配置上提供
    --keep      保留中间目标文件（调试用）
    -v          打印详细过程

依赖
    Python 3.9 以上；`pip install ziglang==0.17.1`（Zig 编译器以 Python 包形式提供）
"""

import argparse
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import memmap

BASE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(BASE, "src")
INC = os.path.join(BASE, "include")
LD = os.path.join(BASE, "ld")
CRT = os.path.join(LD, "crt0.S")
def link_script(map_key):
    return os.path.join(LD, "link16.ld" if map_key == "16m" else "link1g.ld")


LINK = link_script(memmap.DEFAULT)
SOFTFLOAT = os.path.join(SRC, "softfloat.c")
ELF2TC = os.path.join(BASE, "elf2tc.py")

# 目标平台与固定开关。TARGET 两条 + BASE_FLAGS 五条，一个都不能少，原因见第六章正文。
TARGET = ["--target=riscv32-freestanding-none", "-mcpu=generic_rv32+m"]
BASE_FLAGS = ["-ffreestanding", "-nostdlib", "-fno-sanitize=all",
              "-fno-stack-protector", "-I" + INC]

# 平台自带的一套源文件（每个程序都要带上）
PLATFORM = [os.path.join(SRC, f) for f in
            ("tcrv32.c", "support.c", "softfloat.c", "malloc.c")]

# GPU 驱动（只有 GPU 程序需要）。
# 不放进 PLATFORM：放进去会让**所有**镜像都变大，破坏"镜像与验收记录一致"。
# 另外 GPU 是 1 GiB 配置的硬件变体（TCRV32-withGPU），16m 配置里没有这个设备。
GPU_SRC = os.path.join(SRC, "gpu.c")


def platform_sources(gpu=False):
    """标准一套；gpu=True 时追加 gpu.c。"""
    return list(PLATFORM) + ([GPU_SRC] if gpu else [])


def opt_for(path):
    """softfloat.c 必须 -O0，其余 -Os。原因见第六章 §5。"""
    if os.path.normcase(os.path.abspath(path)) == os.path.normcase(os.path.abspath(SOFTFLOAT)):
        return "-O0"
    return "-Os"


def run(cmd):
    p = subprocess.run(cmd, capture_output=True, text=True,
                       errors="replace", encoding="utf-8")
    return p.returncode, (p.stdout or "") + (p.stderr or "")


def build(sources, out_bin, out_elf=None, pad=None, keep=False, verbose=False,
          map_key=None, gpu=False):
    """把 sources（C 或汇编）加上平台源文件编成一个镜像。返回 0 表示成功。"""
    map_key = map_key or memmap.DEFAULT
    if gpu and map_key != "1g":
        return 2, ("GPU 只在 1 GiB 配置（--map 1g）上提供；当前是 --map %s。\n"
                   "16 MiB 配置没有 GPU 设备（硬件变体只有 TCRV32-withGPU 一套）。"
                   % map_key)
    defines = ["-D" + memmap.get(map_key).define]
    tmp = tempfile.mkdtemp(prefix="tcrv32_") if not keep else os.path.join(BASE, "obj")
    os.makedirs(tmp, exist_ok=True)
    objdir = tmp

    objs = []
    for s in [CRT] + platform_sources(gpu) + list(sources):
        tag = os.path.basename(s).replace(".", "_") + opt_for(s).replace("-", "") + ".o"
        o = os.path.join(objdir, tag)
        cmd = [sys.executable, "-m", "ziglang", "cc"] + TARGET + [opt_for(s)] + \
              BASE_FLAGS + defines + ["-c", s, "-o", o]
        rc, msg = run(cmd)
        if rc != 0:
            return rc, "编译 %s 失败：\n%s" % (os.path.basename(s), msg[:1500])
        objs.append(o)

    elf = out_elf or os.path.join(objdir, "out.elf")
    cmd = [sys.executable, "-m", "ziglang", "cc"] + TARGET + \
          ["-nostdlib", "-Wl,-T," + link_script(map_key), "-o", elf] + objs
    rc, msg = run(cmd)
    if rc != 0:
        errs = [l for l in msg.split("\n") if "error" in l.lower() or "undefined" in l]
        return rc, "链接失败：\n" + "\n".join(errs[:12])

    cmd = [sys.executable, ELF2TC, elf, "-o", out_bin, "--map", map_key]
    if pad:
        cmd += ["--pad-to", str(pad)]
    rc, msg = run(cmd)
    if rc != 0:
        return rc, "elf2tc 失败：\n" + msg[:1200]

    if verbose:
        print("  镜像：%s（%d 字节）" % (out_bin, os.path.getsize(out_bin)))
    return 0, ""


def main():
    ap = argparse.ArgumentParser(description="TCRV32 一键构建")
    ap.add_argument("sources", nargs="+", help="C 或汇编源文件")
    ap.add_argument("-o", dest="out", required=True, help="输出的 .bin 路径")
    ap.add_argument("--elf", default=None, help="保留 ELF 到此路径")
    ap.add_argument("--pad", default=None, help="补齐到指定字节数")
    ap.add_argument("--gpu", action="store_true",
                    help="额外编入 GPU 驱动 src/gpu.c（只在 1 GiB 配置上可用）")
    ap.add_argument("--keep", action="store_true", help="保留中间文件")
    ap.add_argument("-v", action="store_true", help="打印详细过程")
    memmap.add_arg(ap)
    a = ap.parse_args()

    pad = int(a.pad, 0) if a.pad else None
    os.makedirs(os.path.dirname(os.path.abspath(a.out)) or ".", exist_ok=True)
    rc, msg = build(a.sources, a.out, a.elf, pad, a.keep, a.v,
                    map_key=a.map, gpu=a.gpu)
    if rc:
        print(msg, file=sys.stderr)
        return 1
    print("构建成功：%s（%d 字节）" % (a.out, os.path.getsize(a.out)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
