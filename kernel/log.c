#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"

// 允许并发 FS 系统调用的简单日志系统。
//
// 一个日志事务包含多个 FS 系统调用的更新。
// 日志系统仅在没有活跃的 FS 系统调用时
// 才提交。因此永远不需要推理
// 某次提交是否会写入未提交的
// 系统调用更新到磁盘。
//
// 系统调用应调用 begin_op()/end_op() 来标记
// 其开始和结束。通常 begin_op() 只是增加
// 进行中的 FS 系统调用计数并返回。
// 但如果它认为日志空间即将耗尽，
// 它会睡眠直到最后一个未完成的 end_op() 提交。
//
// 日志是一个物理重做日志，包含磁盘块。
// 磁盘上的日志格式：
//   头块，包含块 A、B、C 等的块号
//   块 A
//   块 B
//   块 C
//   ...
// 日志追加是同步的。

// 头块的内容，同时用于磁盘上的头块
// 和在内存中跟踪提交前已记录的块号。
struct logheader {
  int n;
  int block[LOGBLOCKS];
};

struct log {
  struct spinlock lock;
  int start;
  int outstanding; // 有多少 FS 系统调用正在执行。
  int committing;  // 正在 commit() 中，请等待。
  int dev;
  struct logheader lh;
};
struct log log;

static void recover_from_log(void);
static void commit();

void
initlog(int dev, struct superblock *sb)
{
  if (sizeof(struct logheader) >= BSIZE)
    panic("initlog: too big logheader");

  initlock(&log.lock, "log");
  log.start = sb->logstart;
  log.dev = dev;
  recover_from_log();
}

// 将已提交的块从日志拷贝到它们的主位置
static void
install_trans(int recovering)
{
  int tail;

  for (tail = 0; tail < log.lh.n; tail++) {
    if(recovering) {
      printf("recovering tail %d dst %d\n", tail, log.lh.block[tail]);
    }
    struct buf *lbuf = bread(log.dev, log.start+tail+1); // 读取日志块
    struct buf *dbuf = bread(log.dev, log.lh.block[tail]); // 读取目标块
    memmove(dbuf->data, lbuf->data, BSIZE);  // 将块拷贝到目标
    bwrite(dbuf);  // 将目标写入磁盘
    if(recovering == 0)
      bunpin(dbuf);
    brelse(lbuf);
    brelse(dbuf);
  }
}

// 从磁盘读取日志头到内存中的日志头
static void
read_head(void)
{
  struct buf *buf = bread(log.dev, log.start);
  struct logheader *lh = (struct logheader *) (buf->data);
  int i;
  log.lh.n = lh->n;
  for (i = 0; i < log.lh.n; i++) {
    log.lh.block[i] = lh->block[i];
  }
  brelse(buf);
}

// 将内存中的日志头写入磁盘。
// 这是当前事务
// 真正提交的时刻。
static void
write_head(void)
{
  struct buf *buf = bread(log.dev, log.start);
  struct logheader *hb = (struct logheader *) (buf->data);
  int i;
  hb->n = log.lh.n;
  for (i = 0; i < log.lh.n; i++) {
    hb->block[i] = log.lh.block[i];
  }
  bwrite(buf);
  brelse(buf);
}

static void
recover_from_log(void)
{
  read_head();
  install_trans(1); // 如果已提交，从日志拷贝到磁盘
  log.lh.n = 0;
  write_head(); // 清除日志
}

// 在每个 FS 系统调用开始时调用。
void
begin_op(void)
{
  acquire(&log.lock);
  while(1){
    if(log.committing){
      sleep(&log, &log.lock);
    } else if(log.lh.n + (log.outstanding+1)*MAXOPBLOCKS > LOGBLOCKS){
      // 此操作可能耗尽日志空间；等待提交。
      sleep(&log, &log.lock);
    } else {
      log.outstanding += 1;
      release(&log.lock);
      break;
    }
  }
}

// 在每个 FS 系统调用结束时调用。
// 如果这是最后一个未完成的操作，则提交。
void
end_op(void)
{
  int do_commit = 0;

  acquire(&log.lock);
  log.outstanding -= 1;
  if(log.committing)
    panic("log.committing");
  if(log.outstanding == 0){
    do_commit = 1;
    log.committing = 1;
  } else {
    // begin_op() 可能在等待日志空间，
    // 递减 log.outstanding 已经减少了
    // 预留空间量。
    wakeup(&log);
  }
  release(&log.lock);

  if(do_commit){
    // 调用 commit 但不持有锁，因为不允许
    // 持锁睡眠。
    commit();
    acquire(&log.lock);
    log.committing = 0;
    wakeup(&log);
    release(&log.lock);
  }
}

// 将修改后的块从缓存拷贝到日志。
static void
write_log(void)
{
  int tail;

  for (tail = 0; tail < log.lh.n; tail++) {
    struct buf *to = bread(log.dev, log.start+tail+1); // 日志块
    struct buf *from = bread(log.dev, log.lh.block[tail]); // 缓存块
    memmove(to->data, from->data, BSIZE);
    bwrite(to);  // write the log
    brelse(from);
    brelse(to);
  }
}

static void
commit()
{
  if (log.lh.n > 0) {
    write_log();     // 将修改后的块从缓存写入日志
    write_head();    // 将头写入磁盘 —— 真正的提交
    install_trans(0); // 现在将写入安装到主位置
    log.lh.n = 0;
    write_head();    // 从日志中擦除该事务
  }
}

// 调用者已修改 b->data 且不再使用该缓冲区。
// 记录块号并通过增加 refcnt 将其固定在缓存中。
// commit()/write_log() 将执行磁盘写入。
//
// log_write() 替代 bwrite()；典型用法为：
//   bp = bread(...)
//   修改 bp->data[]
//   log_write(bp)
//   brelse(bp)
void
log_write(struct buf *b)
{
  int i;

  acquire(&log.lock);
  if (log.lh.n >= LOGBLOCKS)
    panic("too big a transaction");
  if (log.outstanding < 1)
    panic("log_write outside of trans");

  for (i = 0; i < log.lh.n; i++) {
    if (log.lh.block[i] == b->blockno)   // 日志吸收
      break;
  }
  log.lh.block[i] = b->blockno;
  if (i == log.lh.n) {  // 添加新块到日志？
    bpin(b);
    log.lh.n++;
  }
  release(&log.lock);
}

