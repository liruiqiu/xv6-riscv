#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"

void main();
void timerinit();

// entry.S 需要为每个 CPU 分配一个栈。
__attribute__ ((aligned (16))) char stack0[4096 * NCPU];

// entry.S 在 machine 模式下跳转至此，运行于 stack0 上。
void
start()
{
  // 将 M 先前特权模式设为 Supervisor，用于 mret。
  unsigned long x = r_mstatus();
  x &= ~MSTATUS_MPP_MASK;
  x |= MSTATUS_MPP_S;
  w_mstatus(x);

  // 将 M 异常程序计数器设为 main，用于 mret。
  // 需要 gcc -mcmodel=medany
  w_mepc((uint64)main);

  // 暂时禁用分页。
  w_satp(0);

  // 将所有中断和异常委托给 supervisor 模式。
  w_medeleg(0xffff);
  w_mideleg(0xffff);
  w_sie(r_sie() | SIE_SEIE | SIE_STIE);

  // 配置物理内存保护，使 supervisor 模式
  // 可以访问所有物理内存。
  w_pmpaddr0(0x3fffffffffffffull);
  w_pmpcfg0(0xf);

  // 请求时钟中断。
  timerinit();

  // 将每个 CPU 的 hartid 保存在其 tp 寄存器中，供 cpuid() 使用。
  int id = r_mhartid();
  w_tp(id);

  // 切换到 supervisor 模式并跳转到 main()。
  asm volatile("mret");
}

// 让每个 hart 产生定时器中断。
void
timerinit()
{
  // 启用 supervisor 模式定时器中断。
  w_mie(r_mie() | MIE_STIE);

  // 启用 sstc 扩展（即 stimecmp）。
  w_menvcfg(r_menvcfg() | (1L << 63));

  // 允许 supervisor 使用 stimecmp 和 time。
  w_mcounteren(r_mcounteren() | 2);

  // 请求第一个时钟中断。
  w_stimecmp(r_time() + 1000000);
}
