        #
        # 在 supervisor 模式下发生的中断和异常
        # 会来到这里。
        #
        # 当前栈是内核栈。
        # 保存寄存器，调用 kerneltrap()。
        # 当 kerneltrap() 返回时，恢复寄存器，返回。
        #
.globl kerneltrap
.globl kernelvec
.align 4
kernelvec:
        # 腾出空间保存寄存器。
        addi sp, sp, -256

        # 保存调用者保存寄存器。
        sd ra, 0(sp)
        # sd sp, 8(sp)
        sd gp, 16(sp)
        sd tp, 24(sp)
        sd t0, 32(sp)
        sd t1, 40(sp)
        sd t2, 48(sp)
        sd a0, 72(sp)
        sd a1, 80(sp)
        sd a2, 88(sp)
        sd a3, 96(sp)
        sd a4, 104(sp)
        sd a5, 112(sp)
        sd a6, 120(sp)
        sd a7, 128(sp)
        sd t3, 216(sp)
        sd t4, 224(sp)
        sd t5, 232(sp)
        sd t6, 240(sp)

        # 调用 trap.c 中的 C 陷阱处理函数
        call kerneltrap

        # 恢复寄存器。
        ld ra, 0(sp)
        # ld sp, 8(sp)
        ld gp, 16(sp)
        # 不恢复 tp（包含 hartid），以防我们被迁移到其他 CPU
        ld t0, 32(sp)
        ld t1, 40(sp)
        ld t2, 48(sp)
        ld a0, 72(sp)
        ld a1, 80(sp)
        ld a2, 88(sp)
        ld a3, 96(sp)
        ld a4, 104(sp)
        ld a5, 112(sp)
        ld a6, 120(sp)
        ld a7, 128(sp)
        ld t3, 216(sp)
        ld t4, 224(sp)
        ld t5, 232(sp)
        ld t6, 240(sp)

        addi sp, sp, 256

        # 返回到内核中之前正在执行的代码。
        sret
