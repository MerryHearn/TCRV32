/* TCRV32 v0.1 链接脚本 —— 与 ../RV32IM-ISA规范.md §2.3 一致
 *
 * 内存地图（1 GiB 单块物理 RAM，MMIO 在 RAM 之外）：
 *   IMEM    0x00000000  16 MiB   指令（复位向量在此）
 *   DMEM    0x01000000  256 MiB  数据/堆/栈（栈顶 = 0x11000000）
 *   ttext   0x11000000  16 MiB   文本 VRAM
 *   pixel   0x12000000  64 MiB   像素 VRAM
 *   assets  0x16000000  64 MiB   只读资源
 *   free    0x1A000000  608 MiB  空闲（程序可自用）
 *   MMIO    0x40000000  4 KiB    外设寄存器（恰在 1 GiB 边界上）
 */
OUTPUT_ARCH(riscv)
ENTRY(_start)

MEMORY
{
  IMEM (rx)  : ORIGIN = 0x00000000, LENGTH = 16M
  DMEM (rwx) : ORIGIN = 0x01000000, LENGTH = 256M
}

_stack_top = ORIGIN(DMEM) + LENGTH(DMEM);      /* 0x11000000 */

SECTIONS
{
  .text : {
    KEEP(*(.text.start))        /* 复位向量必须落在 0x0 */
    *(.text*)
  } > IMEM

  /* .rodata 单独出一个输出节（仍在 IMEM）：这样 .text 里只有指令，
   * rvcheck/elf2tc 的"每个字都必须 inst[1:0]==11"校验才有意义。
   * 硬件侧后果：取指可能落在 .rodata 上（如果 PC 跑飞），不能当成"译码错误"提前判定。 */
  .rodata : {
    *(.rodata*)
    *(.srodata*)
    . = ALIGN(4);
  } > IMEM

  /* .data 的加载地址(LMA)在 IMEM，运行地址(VMA)在 DMEM —— crt0 负责搬 */
  .data : {
    __data_start = .;
    *(.sdata*)
    *(.data*)
    . = ALIGN(4);
    __data_end = .;
  } > DMEM AT > IMEM
  __data_load = LOADADDR(.data);

  .bss (NOLOAD) : {
    __bss_start = .;
    *(.sbss*)
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    __bss_end = .;
  } > DMEM

  _end = .;

  /* gp 指向 small-data 中部；配合 crt0 的 la gp（也可整体用 -Wl,--no-relax 规避） */
  PROVIDE(__global_pointer$ = __data_start + 0x800);

  /DISCARD/ : { *(.eh_frame*) *(.comment) *(.riscv.attributes) }
}
