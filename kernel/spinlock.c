// 互斥自旋锁。

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "proc.h"
#include "defs.h"

void
initlock(struct spinlock *lk, char *name)
{
  lk->name = name;
  lk->locked = 0;
  lk->cpu = 0;
}

// 获取锁。
// 循环（自旋）直到获取到锁。
void
acquire(struct spinlock *lk)
{
  push_off(); // 禁用中断以避免死锁。
  if(holding(lk))
    panic("acquire");

  // 在 RISC-V 上，sync_lock_test_and_set 转换为原子交换：
  //   a5 = 1
  //   s1 = &lk->locked
  //   amoswap.w.aq a5, a5, (s1)
  while(__sync_lock_test_and_set(&lk->locked, 1) != 0)
    ;

  // 告知 C 编译器和处理器不要将加载或存储指令
  // 移动到此点之后，以确保临界区中的内存
  // 引用严格发生在锁被获取之后。
  // 在 RISC-V 上，这会生成一条 fence 指令。
  __sync_synchronize();

  // 记录锁获取信息，供 holding() 和调试使用。
  lk->cpu = mycpu();
}

// 释放锁。
void
release(struct spinlock *lk)
{
  if(!holding(lk))
    panic("release");

  lk->cpu = 0;

  // 告知 C 编译器和 CPU 不要将加载或存储指令
  // 移动到此点之后，以确保临界区中的所有存储
  // 在锁释放之前对其他 CPU 可见，
  // 同时确保临界区中的加载严格发生在
  // 锁释放之前。
  // 在 RISC-V 上，这会生成一条 fence 指令。
  __sync_synchronize();

  // 释放锁，等效于 lk->locked = 0。
  // 此代码不使用 C 赋值语句，因为 C 标准
  // 意味着赋值可能被实现为
  // 多条存储指令。
  // 在 RISC-V 上，sync_lock_release 转换为原子交换：
  //   s1 = &lk->locked
  //   amoswap.w zero, zero, (s1)
  __sync_lock_release(&lk->locked);

  pop_off();
}

// 检查当前 CPU 是否持有该锁。
// 调用前必须禁用中断。
int
holding(struct spinlock *lk)
{
  int r;
  r = (lk->locked && lk->cpu == mycpu());
  return r;
}

// push_off/pop_off 类似于 intr_off()/intr_on()，但它们是配对的：
// 需要两次 pop_off() 才能撤销两次 push_off()。此外，如果中断
// 初始为关闭状态，则 push_off, pop_off 之后它们仍然保持关闭。

void
push_off(void)
{
  int old = intr_get();

  // 禁用中断以避免在使用 mycpu() 时
  // 发生非自愿上下文切换。
  intr_off();

  if(mycpu()->noff == 0)
    mycpu()->intena = old;
  mycpu()->noff += 1;
}

void
pop_off(void)
{
  struct cpu *c = mycpu();
  if(intr_get())
    panic("pop_off - interruptible");
  if(c->noff < 1)
    panic("pop_off");
  c->noff -= 1;
  if(c->noff == 0 && c->intena)
    intr_on();
}
