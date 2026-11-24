/* TCRV32 v0.1 启动代码（crt0）
 *
 * 职责：gp → sp → .bss 清零 → .data 从 IMEM 搬到 DMEM → main → exit
 * 与 toolchain/link.ld 的符号约定严格对应，改一处必须同时改另一处。
 *
 * 注意：`la` 在 -mno-relax / -Wl,--no-relax 下会展开为 auipc+addi（PC 相对），
 *      这在 IMEM(0x0) 与 DMEM(0x200000) 分离的地图里仍然正确，因为链接器已算好偏移。
 */
    .section .text.start, "ax", @progbits
    .globl _start
    .type  _start, @function
_start:
    .option push
    .option norelax                 /* la gp 不能被松弛成 gp 相对寻址 */
    la   gp, __global_pointer$      /* small-data 指针（用 --no-relax 时可不设） */
    .option pop

    la   sp, _stack_top             /* 0x00400000，向低地址增长，16 字节对齐 */
    andi sp, sp, -16

    /* ---- 1. .bss 清零 ---- */
    la   t0, __bss_start
    la   t1, __bss_end
1:  bgeu t0, t1, 2f
    sw   zero, 0(t0)
    addi t0, t0, 4
    j    1b

    /* ---- 2. .data 从 IMEM 的加载镜像搬到 DMEM 运行地址 ---- */
2:  la   t0, __data_load          /* LMA：在 IMEM 里 */
    la   t1, __data_start         /* VMA：在 DMEM 里 */
    la   t2, __data_end
3:  bgeu t1, t2, 4f
    lw   t3, 0(t0)
    sw   t3, 0(t1)
    addi t0, t0, 4
    addi t1, t1, 4
    j    3b

    /* ---- 3. 进 main，返回值当退出码 ---- */
4:  call main
    li   a7, 60                   /* SYS_exit */
    ecall

5:  j    5b                        /* 理论上到不了这里 */
    .size _start, .-_start
