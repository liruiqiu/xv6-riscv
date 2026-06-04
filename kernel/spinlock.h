// 互斥锁。
struct spinlock {
  uint locked;       // 锁是否已被持有？

  // 用于调试：
  char *name;        // 锁的名称。
  struct cpu *cpu;   // 持有该锁的 CPU。
};

