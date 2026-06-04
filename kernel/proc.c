#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;

extern void forkret(void);
static void freeproc(struct proc *p);

extern char trampoline[]; // trampoline.S

// 帮助确保 wait() 的父进程的唤醒
// 不会丢失。使用 p->parent 时帮助遵守
// 内存模型。
// 必须在任何 p->lock 之前获取。
struct spinlock wait_lock;

// 为每个进程的内核栈分配一页。
// 将其映射到内存高处，后跟一个无效的
// 保护页。
void
proc_mapstacks(pagetable_t kpgtbl)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    char *pa = kalloc();
    if(pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int) (p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

// 初始化进程表。
void
procinit(void)
{
  struct proc *p;

  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
  for(p = proc; p < &proc[NPROC]; p++) {
      initlock(&p->lock, "proc");
      p->state = UNUSED;
      p->kstack = KSTACK((int) (p - proc));
  }
}

// 必须在中断禁用时调用，
// 以防止进程被移动到不同 CPU
// 的竞争条件。
int
cpuid()
{
  int id = r_tp();
  return id;
}

// 返回此 CPU 的 cpu 结构体。
// 必须禁用中断。
struct cpu*
mycpu(void)
{
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// 返回当前 struct proc *，如果没有则返回零。
struct proc*
myproc(void)
{
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

int
allocpid()
{
  int pid;
  
  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

// 在进程表中查找 UNUSED 状态的进程。
// 如果找到，初始化在内核中运行所需的状态，
// 并持有 p->lock 返回。
// 如果没有空闲进程，或内存分配失败，返回 0。
static struct proc*
allocproc(void)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state == UNUSED) {
      goto found;
    } else {
      release(&p->lock);
    }
  }
  return 0;

found:
  p->pid = allocpid();
  p->state = USED;

  // 分配 trapframe 页。
  if((p->trapframe = (struct trapframe *)kalloc()) == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // 一个空的用户页表。
  p->pagetable = proc_pagetable(p);
  if(p->pagetable == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // 设置新上下文以从 forkret 开始执行，
  // forkret 将返回用户空间。
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;

  return p;
}

// 释放 proc 结构体及其关联的数据，
// 包括用户页。
// 调用者必须持有 p->lock。
static void
freeproc(struct proc *p)
{
  if(p->trapframe)
    kfree((void*)p->trapframe);
  p->trapframe = 0;
  if(p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->state = UNUSED;
}

// 为给定进程创建用户页表，不含用户内存，
// 但包含 trampoline 和 trapframe 页。
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // 一个空页表。
  pagetable = uvmcreate();
  if(pagetable == 0)
    return 0;

  // 将 trampoline 代码（用于系统调用返回）
  // 映射到最高用户虚拟地址。
  // 仅 supervisor 在进出用户空间时使用它，
  // 因此不加 PTE_U。
  if(mappages(pagetable, TRAMPOLINE, PGSIZE,
              (uint64)trampoline, PTE_R | PTE_X) < 0){
    uvmfree(pagetable, 0);
    return 0;
  }

  // 将 trapframe 页映射到 trampoline 页正下方，供
  // trampoline.S 使用。
  if(mappages(pagetable, TRAPFRAME, PGSIZE,
              (uint64)(p->trapframe), PTE_R | PTE_W) < 0){
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// 释放进程的页表，并释放
// 其引用的物理内存。
void
proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
}

// 设置第一个用户进程。
void
userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;

  p->cwd = namei("/");

  p->state = RUNNABLE;

  release(&p->lock);
}

// 将用户内存增长或缩减 n 字节。
// 成功返回 0，失败返回 -1。
int
growproc(int n)
{
  uint64 sz;
  struct proc *p = myproc();

  sz = p->sz;
  if(n > 0){
    if(sz + n > TRAPFRAME) {
      return -1;
    }
    if((sz = uvmalloc(p->pagetable, sz, sz + n, PTE_W)) == 0) {
      return -1;
    }
  } else if(n < 0){
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}

// 创建一个新进程，复制父进程。
// 设置子进程内核栈，使其如同从 fork() 系统调用返回。
int
kfork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // 分配进程。
  if((np = allocproc()) == 0){
    return -1;
  }

  // 将用户内存从父进程复制到子进程。
  if(uvmcopy(p->pagetable, np->pagetable, p->sz) < 0){
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  // 复制保存的用户寄存器。
  *(np->trapframe) = *(p->trapframe);

  // 使 fork 在子进程中返回 0。
  np->trapframe->a0 = 0;

  // 增加打开文件描述符的引用计数。
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->lock);
  np->state = RUNNABLE;
  release(&np->lock);

  return pid;
}

// 将 p 的遗弃子进程交给 init。
// 调用者必须持有 wait_lock。
void
reparent(struct proc *p)
{
  struct proc *pp;

  for(pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->parent == p){
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// 退出当前进程。不返回。
// 已退出的进程保持 zombie 状态
// 直到其父进程调用 wait()。
void
kexit(int status)
{
  struct proc *p = myproc();

  if(p == initproc)
    panic("init exiting");

  // 关闭所有打开的文件。
  for(int fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd]){
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  acquire(&wait_lock);

  // 将所有子进程交给 init。
  reparent(p);

  // 父进程可能在 wait() 中睡眠。
  wakeup(p->parent);

  acquire(&p->lock);

  p->xstate = status;
  p->state = ZOMBIE;

  release(&wait_lock);

  // 跳入调度器，永不返回。
  sched();
  panic("zombie exit");
}

// 等待子进程退出并返回其 pid。
// 如果此进程没有子进程则返回 -1。
int
kwait(uint64 addr)
{
  struct proc *pp;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for(;;){
    // 扫描进程表，查找已退出的子进程。
    havekids = 0;
    for(pp = proc; pp < &proc[NPROC]; pp++){
      if(pp->parent == p){
        // 确保子进程不在 exit() 或 swtch() 中。
        acquire(&pp->lock);

        havekids = 1;
        if(pp->state == ZOMBIE){
          // 找到一个。
          pid = pp->pid;
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&pp->xstate,
                                  sizeof(pp->xstate)) < 0) {
            release(&pp->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(pp);
          release(&pp->lock);
          release(&wait_lock);
          return pid;
        }
        release(&pp->lock);
      }
    }

    // 如果没有子进程，等待没有意义。
    if(!havekids || killed(p)){
      release(&wait_lock);
      return -1;
    }

    // 等待子进程退出。
    sleep(p, &wait_lock);  //DOC: wait-sleep
  }
}

// 每个 CPU 的进程调度器。
// 每个 CPU 在完成自身设置后调用 scheduler()。
// 调度器永不返回。它循环执行：
//  - 选择一个要运行的进程。
//  - 通过 swtch 开始运行该进程。
//  - 最终该进程通过 swtch 将控制权
//    交还给调度器。
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();

  c->proc = 0;
  for(;;){
    // 最近运行的进程可能关闭了中断；
    // 启用它们以避免在所有进程都在等待时
    // 发生死锁。然后再次关闭它们
    // 以避免中断和 wfi 之间可能的竞争。
    intr_on();
    intr_off();

    int found = 0;
    for(p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);
      if(p->state == RUNNABLE) {
        // 切换到选中的进程。该进程的责任是
        // 释放其锁，然后在跳回给我们之前
        // 重新获取它。
        p->state = RUNNING;
        c->proc = p;
        swtch(&c->context, &p->context);

        // 进程暂时运行完毕。
        // 它应该在返回之前已更改其 p->state。
        c->proc = 0;
        found = 1;
      }
      release(&p->lock);
    }
    if(found == 0) {
      // 无任务可运行；在此核心上停止运行，直到有中断。
      asm volatile("wfi");
    }
  }
}

// 切换到调度器。必须仅持有 p->lock
// 且已更改 proc->state。保存并恢复
// intena，因为 intena 是此内核线程的属性，
// 而非此 CPU 的属性。它本应是
// proc->intena 和 proc->noff，但这会在
// 少数持有锁但没有进程的地方
// 导致问题。
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&p->lock))
    panic("sched p->lock");
  if(mycpu()->noff != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched RUNNING");
  if(intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// 放弃 CPU 一个调度轮次。
void
yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;
  sched();
  release(&p->lock);
}

// fork 子进程首次被 scheduler() 调度时
// 将 swtch 到 forkret。
void
forkret(void)
{
  extern char userret[];
  static int first = 1;
  struct proc *p = myproc();

  // 仍持有来自调度器的 p->lock。
  release(&p->lock);

  if (first) {
    // 文件系统初始化必须在常规进程的上下文中运行
    // （例如因为它调用 sleep），因此不能
    // 从 main() 中运行。
    fsinit(ROOTDEV);

    first = 0;
    // 确保其他核心看到 first=0。
    __sync_synchronize();

    // 文件系统初始化完成后，我们可以调用 kexec()。
    // 将 kexec 的返回值 (argc) 放入 a0。
    p->trapframe->a0 = kexec("/init", (char *[]){ "/init", 0 });
    if (p->trapframe->a0 == -1) {
      panic("exec");
    }
  }

  // 返回用户空间，模拟 usertrap() 的返回。
  prepare_return();
  uint64 satp = MAKE_SATP(p->pagetable);
  uint64 trampoline_userret = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64))trampoline_userret)(satp);
}

// 在通道 chan 上睡眠，释放条件锁 lk。
// 被唤醒时重新获取 lk。
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();

  // 必须获取 p->lock 才能
  // 更改 p->state 然后调用 sched。
  // 一旦持有 p->lock，我们可以
  // 保证不会错过任何唤醒
  // （wakeup 会锁定 p->lock），
  // 因此可以安全地释放 lk。

  acquire(&p->lock);  //DOC: sleeplock1
  release(lk);

  // 进入睡眠。
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // 清理。
  p->chan = 0;

  // 重新获取原始锁。
  release(&p->lock);
  acquire(lk);
}

// 唤醒所有在通道 chan 上睡眠的进程。
// 调用者应持有条件锁。
void
wakeup(void *chan)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    if(p != myproc()){
      acquire(&p->lock);
      if(p->state == SLEEPING && p->chan == chan) {
        p->state = RUNNABLE;
      }
      release(&p->lock);
    }
  }
}

// 终止具有给定 pid 的进程。
// 受害者直到尝试返回用户空间时才会退出
// （参见 trap.c 中的 usertrap()）。
int
kkill(int pid)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid){
      p->killed = 1;
      if(p->state == SLEEPING){
        // 将进程从 sleep() 中唤醒。
        p->state = RUNNABLE;
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

void
setkilled(struct proc *p)
{
  acquire(&p->lock);
  p->killed = 1;
  release(&p->lock);
}

int
killed(struct proc *p)
{
  int k;
  
  acquire(&p->lock);
  k = p->killed;
  release(&p->lock);
  return k;
}

// 复制到用户地址或内核地址，
// 取决于 usr_dst。
// 成功返回 0，错误返回 -1。
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if(user_dst){
    return copyout(p->pagetable, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// 从用户地址或内核地址复制，
// 取决于 usr_src。
// 成功返回 0，错误返回 -1。
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if(user_src){
    return copyin(p->pagetable, dst, src, len);
  } else {
    memmove(dst, (char*)src, len);
    return 0;
  }
}

// 将进程列表打印到控制台。用于调试。
// 当用户在控制台键入 ^P 时运行。
// 不加锁以避免进一步卡住已经卡死的机器。
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [USED]      "used",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  struct proc *p;
  char *state;

  printf("\n");
  for(p = proc; p < &proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d %s %s", p->pid, state, p->name);
    printf("\n");
  }
}
