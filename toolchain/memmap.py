#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""memmap.py —— 两套内存地图的唯一真源（16 MiB 与 1 GiB）

为什么要有这个文件：
    本项目同时维护"小内存"与"大内存"两套地图。地址、容量、栈顶这些东西
    分散在头文件、链接脚本、装载器、模拟器四处，任何一处不同步都会变成
    "链接通过、运行即死"的隐性错误。所以把它们集中到这一处，
    其余三处都从这里取数。

两套地图的差别只有下面这些数字，结构完全一样：
    16m   IMEM 1 MiB / DMEM 2 MiB / 文本 1 MiB / 像素 4 MiB / 资源 4 MiB，栈顶 0x00400000
    1g    IMEM 16 MiB / DMEM 256 MiB / 文本 16 MiB / 像素 64 MiB / 资源 64 MiB / 空闲 608 MiB，
          栈顶 0x11000000

用法：
    from memmap import get
    m = get("16m")          # 或 "1g"
    m.mem_size, m.regions, m.stack_top, m.cc_define
"""

PROFILES = {
    "16m": {
        "label": "16 MiB",
        "mem_size": 0x0100_0000,
        "stack_top": 0x0040_0000,
        "define": "TCRV32_MAP_16M",
        # (名字, 起始, 大小, 可执行, 可写)
        "regions": [
            ("imem",   0x0000_0000, 0x0010_0000, True,  False),
            ("dmem",   0x0020_0000, 0x0020_0000, False, True),
            ("ttext",  0x0040_0000, 0x0010_0000, False, True),
            ("pixel",  0x0080_0000, 0x0040_0000, False, True),
            ("assets", 0x00C0_0000, 0x0040_0000, False, False),
        ],
        "vram_base": 0x0040_0000,
        "fb0": 0x0080_0000,
        "fb_back": 0x0081_0000,      # +64 KiB
        "fb1": 0x0082_0000,          # +128 KiB
        "fb_stride": 0x0001_0000,    # 每屏预留 64 KiB
        "scratch": 0x003E_0000,      # 程序自用的固定小格子（原来的 FRAME_CNT/STATE 就放这）
        "heap_reserve": 8 * 1024,    # 给栈留 8 KiB
    },
    "1g": {
        "label": "1 GiB",
        "mem_size": 0x4000_0000,
        "stack_top": 0x1100_0000,
        "define": "TCRV32_MAP_1G",
        "regions": [
            ("imem",   0x0000_0000, 0x0100_0000, True,  False),
            ("dmem",   0x0100_0000, 0x1000_0000, False, True),
            ("ttext",  0x1100_0000, 0x0100_0000, False, True),
            ("pixel",  0x1200_0000, 0x0400_0000, False, True),
            ("assets", 0x1600_0000, 0x0400_0000, False, False),
            ("free",   0x1A00_0000, 0x2600_0000, False, True),
        ],
        "vram_base": 0x1100_0000,
        "fb0": 0x1200_0000,
        "fb_back": 0x1240_0000,      # +4 MiB，够 1024×768×4
        "fb1": 0x1280_0000,
        "fb_stride": 0x0040_0000,
        "scratch": 0x1000_0000,
        "heap_reserve": 1024 * 1024, # 给栈留 1 MiB
    },
}

DEFAULT = "1g"
NAMES = ("16m", "1g")


class Map:
    def __init__(self, key):
        if key not in PROFILES:
            raise SystemExit("未知内存地图 '%s'，可选：%s" % (key, ", ".join(NAMES)))
        d = PROFILES[key]
        self.key = key
        self.label = d["label"]
        self.mem_size = d["mem_size"]
        self.stack_top = d["stack_top"]
        self.define = d["define"]
        self.regions = [tuple(r) for r in d["regions"]]
        self.vram_base = d["vram_base"]
        self.fb0 = d["fb0"]
        self.fb_back = d["fb_back"]
        self.fb1 = d["fb1"]
        self.fb_stride = d["fb_stride"]
        self.scratch = d["scratch"]
        self.heap_reserve = d["heap_reserve"]

    def __repr__(self):
        return "<memmap %s (%s)>" % (self.key, self.label)


def get(key=None):
    return Map(key or DEFAULT)


def add_arg(ap):
    """给 argparse 挂一个统一的 --map 选项"""
    ap.add_argument("--map", dest="map", default=DEFAULT, choices=NAMES,
                    help="内存地图配置：16m = 16 MiB（小），1g = 1 GiB（大，默认）")
