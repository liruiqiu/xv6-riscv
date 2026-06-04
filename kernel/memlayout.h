// 物理内存布局

// qemu -machine virt 的设置如下，
// 基于 qemu 的 hw/riscv/virt.c：
//
// 00001000 -- 引导 ROM，由 qemu 提供
// 02000000 -- CLINT
// 0C000000 -- PLIC
// 10000000 -- uart0
// 10001000 -- virtio 磁盘
// 80000000 -- qemu 的引导 ROM 将内核加载到此，
//             然后跳转至此。
// 80000000 之后为未使用的 RAM。

// 内核按如下方式使用物理内存：
// 80000000 -- entry.S，然后是内核代码和数据
// end -- 内核页分配区域的起始
// PHYSTOP -- 内核使用的 RAM 终点

// qemu 将 UART 寄存器放在物理内存的此位置。
#define UART0 0x10000000L
#define UART0_IRQ 10

// virtio mmio 接口
#define VIRTIO0 0x10001000
#define VIRTIO0_IRQ 1

// qemu 将平台级中断控制器（PLIC）放在此位置。
#define PLIC 0x0c000000L
#define PLIC_PRIORITY (PLIC + 0x0)
#define PLIC_PENDING (PLIC + 0x1000)
#define PLIC_SENABLE(hart) (PLIC + 0x2080 + (hart)*0x100)
#define PLIC_SPRIORITY(hart) (PLIC + 0x201000 + (hart)*0x2000)
#define PLIC_SCLAIM(hart) (PLIC + 0x201004 + (hart)*0x2000)

// 内核期望从物理地址 0x80000000 到 PHYSTOP
// 有可用的 RAM，
// 供内核和用户页使用。
#define KERNBASE 0x80000000L
#define PHYSTOP (KERNBASE + 128*1024*1024)

// 将 trampoline 页映射到最高地址，
// 用户和内核空间均如此。
#define TRAMPOLINE (MAXVA - PGSIZE)

// 将内核栈映射到 trampoline 下方，
// 每个栈由无效保护页包围。
#define KSTACK(p) (TRAMPOLINE - ((p)+1)* 2*PGSIZE)

// 用户内存布局。
// 从地址零开始：
//   text
//   原始 data 和 bss
//   固定大小栈
//   可扩展的堆
//   ...
//   TRAPFRAME（p->trapframe，由 trampoline 使用）
//   TRAMPOLINE（与内核中的页面相同）
#define TRAPFRAME (TRAMPOLINE - PGSIZE)
