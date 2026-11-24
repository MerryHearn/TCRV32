# TCRV32 —— 游戏存档 + 编译工具链

这是一个 **TCRV32** 发行包：一台在游戏 *Turing Complete* （版本2.1.324）里搭出来的 RISC-V 机器，
以及给它写程序用的编译工具链。

你拿到手可以做两件事：

- **直接玩**：载入 `save/` 里的存档，机器已经在跑。
- **自己写程序**：用 C 写，编成 `.bin`，装进游戏里的内存组件就能跑。

---

## 一、包里有什么

```
save/          游戏存档（载入即用）
toolchain/     编译工具链，自带一切，不依赖别的东西
README.md      本文件
```

`toolchain/` 里各文件的用途：

| 文件 | 作用 |
|---|---|
| `build.py` | 编译入口：C 源 → `.bin` |
| `elf2tc.py` | 把 ELF 转成游戏能加载的裸镜像（顺带校验地址落位） |
| `rv32asm.py` | 汇编器 / 反汇编器（想手写几条指令探针时用） |
| `memmap.py` | 两套内存配置的地址表 |
| `emu/rv32emu.py` | 模拟器：可以在电脑上先跑一遍，不用进游戏 |
| `include/tcrv32.h` | 唯一的头文件：屏幕、控制台、键盘、GPU、堆 |
| `src/` | 平台实现（屏幕 / 字符串 / 软浮点 / 堆 / GPU 驱动） |
| `ld/` | 链接脚本与启动代码 |

> 工具链里**不含**编译器本体。编译器是 Python 包 `ziglang`，见下一节。
> 当前存档尚未包含落地的GPU硬件。

---

## 二、你要装的唯一一个东西

```powershell
python -m pip install ziglang==0.17.1
python -m ziglang version      # 应输出 0.17.1
```

需要 Python 3.9 或更高。

**版本必须是 0.17.1**：这套工具链的编译开关与软浮点设置都是按它实测校准的。
换成别的版本，可能一切正常，也可能冒出很难查的错误。

---

## 三、五分钟：编第一个程序

在 `toolchain/` 旁边新建 `hello.c`：

```c
#include "tcrv32.h"

int main(void) {
    scr_init(TCRV32_VRAM_BASE, SCR_ASCII8);   /* 让屏幕 0 显示文本显存 */
    con_clear(' ');
    con_puts("HELLO TCRV32\n");
    con_puts("this is my first program\n");
    return 0;
}
```

编译（在放 `hello.c` 的目录里执行）：

```powershell
python toolchain/build.py hello.c -o hello_1g.bin
```

看到 `构建成功：hello_1g.bin（约 23 KB）` 就成了。

先在电脑上跑一遍——注意**终端不会打印任何东西**，因为这个程序只往游戏的屏幕写：

```powershell
python toolchain/emu/rv32emu.py --bin hello_1g.bin --map 1g
python toolchain/emu/rv32emu.py --bin hello_1g.bin --map 1g --dump-mem 0x11000000:96
```

第二条命令把文本显存的前 96 字节打出来，你会看到 `HELLO TCRV32`。

---

## 四、两套内存配置（**默认 1 GiB**）

同一份 C 源码可以编给两种内存配置，由 `--map` 决定；不写就是 `1g`，另一种对应`16m`，是早期搭建时的兼容型

```powershell
python toolchain/build.py hello.c -o hello_16m.bin --map 16m
python toolchain/emu/rv32emu.py --bin hello_16m.bin --map 16m
```

**编译用哪套，运行和装进游戏就必须用什么内存**，否则地址对不上。

换配置只改 `--map`。

## 五、把程序装进游戏

1. 放一块**内存**组件，容量按配置填：1 GiB 填 `1073741824`，16 MiB 填 `16777216`（单位是字节，改完保存）。
2. 初始数据来源选 `File`，`Load file` 指向你编出来的 `.bin`。
3. 字节序选 `Little endian`。
4. 放一块**屏幕**，用探针指向这块内存；字符模式下把偏移填成 `TCRV32_VRAM_BASE`
   （1 GiB 是 `0x11000000`，16 MiB 是 `0x00400000`）。偏移填错的表现是"程序跑了，但屏幕全黑"。
5. 让 CPU 跑起来。

`save/` 里的存档就是把上面这些（组件、连线、程序）配好的成品，**载入存档 = 跳过这五步**。

---

## 六、存档怎么用

1. 把 `save/` 里的文件复制到你游戏版本的存档目录（游戏菜单里通常有"打开存档目录"这一项，
   不同版本路径不同，以菜单为准）。
2. 重启游戏或在存档列表里刷新，存档会出现在自定义关卡 / 沙盒里。
3. 载入即可玩。想换成自己编的程序，只要改内存组件的 `Load file`。
4. **载入别人的存档前先备份自己的存档目录**——同名会被覆盖。

如果 `save/` 是空的，说明你这个包只含工具链，没有附带存档。

---

## 七、写程序时最常用的几组

| 想做什么 | 怎么写 |
|---|---|
| 文本屏幕 | `scr_init(TCRV32_VRAM_BASE, SCR_ASCII8)` → `con_clear(' ')` → `con_puts / con_putch / con_putdec / con_puthex` |
| 像素屏幕 | `scr_init_pixel(0, TCRV32_FB0, SCR_SET_256x192)` + `gfx_target_res(...)`；一个像素一个字节，颜色 `RRRGGGBB` |
| 翻页（双缓冲） | 两块缓冲都画完，再 `scr_set_offset2(0, back)`；只改偏移，不拷像素 |
| 键盘 | `kbd_waiting()` 判断有没有事件 → `kbd_get_event(&code, &down)` 取一条；**读一次就消耗一条** |
| GPU（仅 1 GiB） | 先写参数、再写 `CMD` 触发；提交前必须 `gpu_wait()` |
| 输出到终端 | `putstr / putch / putdec / puthex`（这条走串口，屏幕上不显示） |
| 结束程序 | `return 0` 即可（内部走 `ecall a7=60` 停机） |
| 堆 | `malloc / calloc / realloc / free`（`free` 不做回收，够用且可预测） |

屏幕是 **cp437 单字节码表：只能显示 ASCII**，写中文会变乱码；C 注释里写中文没问题。

---

## 八、许可

- 工具链与平台源码：随本包一同分发，可自由用于你自己的项目。
- 编译器：`ziglang`（上游 Zig 项目，MIT 许可），需你按第二节自行安装。
- 游戏：*Turing Complete*（LevelHead / Matthew Cranor）。本包**不含**游戏本体文件，只含自制存档与自制工具。
