#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"

//
// RISC-V 平台级中断控制器（PLIC）。
//

void
plicinit(void)
{
  // 将所需 IRQ 的优先级设为非零（否则禁用）。
  *(uint32*)(PLIC + UART0_IRQ*4) = 1;
  *(uint32*)(PLIC + VIRTIO0_IRQ*4) = 1;
}

void
plicinithart(void)
{
  int hart = cpuid();

  // 为该 hart 的 S 模式设置
  // uart 和 virtio 磁盘的使能位。
  *(uint32*)PLIC_SENABLE(hart) = (1 << UART0_IRQ) | (1 << VIRTIO0_IRQ);

  // 将此 hart 的 S 模式优先级阈值设为 0。
  *(uint32*)PLIC_SPRIORITY(hart) = 0;
}

// 询问 PLIC 我们应该服务哪个中断。
int
plic_claim(void)
{
  int hart = cpuid();
  int irq = *(uint32*)PLIC_SCLAIM(hart);
  return irq;
}

// 告知 PLIC 我们已处理完此 IRQ。
void
plic_complete(int irq)
{
  int hart = cpuid();
  *(uint32*)PLIC_SCLAIM(hart) = irq;
}
