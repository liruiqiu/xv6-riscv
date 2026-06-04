//
// qemu virtio 磁盘设备的驱动程序。
// 通过 qemu 的 mmio 接口访问 virtio。
//
// qemu ... -drive file=fs.img,if=none,format=raw,id=x0 -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"
#include "virtio.h"

// virtio mmio 寄存器 r 的地址。
#define R(r) ((volatile uint32 *)(VIRTIO0 + (r)))

static struct disk {
  // 一组（非环形）DMA 描述符，驱动程序通过它们
  // 告知设备在何处进行单个磁盘操作的
  // 读和写。共有 NUM 个描述符。
  // 大多数命令由几个描述符组成的
  // "链"（链表）构成。
  struct virtq_desc *desc;

  // 一个环形缓冲区，驱动程序在其中写入
  // 希望设备处理的描述符编号，仅包含
  // 每条链的头描述符。该环有
  // NUM 个元素。
  struct virtq_avail *avail;

  // 一个环形缓冲区，设备在其中写入已完成处理的
  // 描述符编号（仅包含每条链的头描述符）。
  // 共有 NUM 个已用环条目。
  struct virtq_used *used;

  // 我们自己的记录。
  char free[NUM];  // 描述符是否空闲？
  uint16 used_idx; // 我们在 used[2..NUM] 中已检查到此位置。

  // 跟踪进行中的操作信息，
  // 供完成中断到达时使用。
  // 以链的首个描述符索引进行索引。
  struct {
    struct buf *b;
    char status;
  } info[NUM];

  // 磁盘命令头。
  // 与描述符一一对应，方便使用。
  struct virtio_blk_req ops[NUM];

  struct spinlock vdisk_lock;

} disk;

void
virtio_disk_init(void)
{
  uint32 status = 0;

  initlock(&disk.vdisk_lock, "virtio_disk");

  if(*R(VIRTIO_MMIO_MAGIC_VALUE) != 0x74726976 ||
     *R(VIRTIO_MMIO_VERSION) != 2 ||
     *R(VIRTIO_MMIO_DEVICE_ID) != 2 ||
     *R(VIRTIO_MMIO_VENDOR_ID) != 0x554d4551){
    panic("could not find virtio disk");
  }

  // 复位设备
  *R(VIRTIO_MMIO_STATUS) = status;

  // 设置 ACKNOWLEDGE 状态位
  status |= VIRTIO_CONFIG_S_ACKNOWLEDGE;
  *R(VIRTIO_MMIO_STATUS) = status;

  // 设置 DRIVER 状态位
  status |= VIRTIO_CONFIG_S_DRIVER;
  *R(VIRTIO_MMIO_STATUS) = status;

  // 协商特性
  uint64 features = *R(VIRTIO_MMIO_DEVICE_FEATURES);
  features &= ~(1 << VIRTIO_BLK_F_RO);
  features &= ~(1 << VIRTIO_BLK_F_SCSI);
  features &= ~(1 << VIRTIO_BLK_F_CONFIG_WCE);
  features &= ~(1 << VIRTIO_BLK_F_MQ);
  features &= ~(1 << VIRTIO_F_ANY_LAYOUT);
  features &= ~(1 << VIRTIO_RING_F_EVENT_IDX);
  features &= ~(1 << VIRTIO_RING_F_INDIRECT_DESC);
  *R(VIRTIO_MMIO_DRIVER_FEATURES) = features;

  // 告知设备特性协商已完成。
  status |= VIRTIO_CONFIG_S_FEATURES_OK;
  *R(VIRTIO_MMIO_STATUS) = status;

  // 重新读取状态以确保 FEATURES_OK 已设置。
  status = *R(VIRTIO_MMIO_STATUS);
  if(!(status & VIRTIO_CONFIG_S_FEATURES_OK))
    panic("virtio disk FEATURES_OK unset");

  // 初始化队列 0。
  *R(VIRTIO_MMIO_QUEUE_SEL) = 0;

  // 确保队列 0 未被使用。
  if(*R(VIRTIO_MMIO_QUEUE_READY))
    panic("virtio disk should not be ready");

  // 检查最大队列大小。
  uint32 max = *R(VIRTIO_MMIO_QUEUE_NUM_MAX);
  if(max == 0)
    panic("virtio disk has no queue 0");
  if(max < NUM)
    panic("virtio disk max queue too short");

  // 分配并清零队列内存。
  disk.desc = kalloc();
  disk.avail = kalloc();
  disk.used = kalloc();
  if(!disk.desc || !disk.avail || !disk.used)
    panic("virtio disk kalloc");
  memset(disk.desc, 0, PGSIZE);
  memset(disk.avail, 0, PGSIZE);
  memset(disk.used, 0, PGSIZE);

  // 设置队列大小。
  *R(VIRTIO_MMIO_QUEUE_NUM) = NUM;

  // 写入物理地址。
  *R(VIRTIO_MMIO_QUEUE_DESC_LOW) = (uint64)disk.desc;
  *R(VIRTIO_MMIO_QUEUE_DESC_HIGH) = (uint64)disk.desc >> 32;
  *R(VIRTIO_MMIO_DRIVER_DESC_LOW) = (uint64)disk.avail;
  *R(VIRTIO_MMIO_DRIVER_DESC_HIGH) = (uint64)disk.avail >> 32;
  *R(VIRTIO_MMIO_DEVICE_DESC_LOW) = (uint64)disk.used;
  *R(VIRTIO_MMIO_DEVICE_DESC_HIGH) = (uint64)disk.used >> 32;

  // 队列已就绪。
  *R(VIRTIO_MMIO_QUEUE_READY) = 0x1;

  // 所有 NUM 个描述符初始均未使用。
  for(int i = 0; i < NUM; i++)
    disk.free[i] = 1;

  // 告知设备我们已完全就绪。
  status |= VIRTIO_CONFIG_S_DRIVER_OK;
  *R(VIRTIO_MMIO_STATUS) = status;

  // plic.c 和 trap.c 负责处理来自 VIRTIO0_IRQ 的中断。
}

// 找到一个空闲描述符，标记为非空闲，返回其索引。
static int
alloc_desc()
{
  for(int i = 0; i < NUM; i++){
    if(disk.free[i]){
      disk.free[i] = 0;
      return i;
    }
  }
  return -1;
}

// 将描述符标记为空闲。
static void
free_desc(int i)
{
  if(i >= NUM)
    panic("free_desc 1");
  if(disk.free[i])
    panic("free_desc 2");
  disk.desc[i].addr = 0;
  disk.desc[i].len = 0;
  disk.desc[i].flags = 0;
  disk.desc[i].next = 0;
  disk.free[i] = 1;
  wakeup(&disk.free[0]);
}

// 释放一个描述符链。
static void
free_chain(int i)
{
  while(1){
    int flag = disk.desc[i].flags;
    int nxt = disk.desc[i].next;
    free_desc(i);
    if(flag & VRING_DESC_F_NEXT)
      i = nxt;
    else
      break;
  }
}

// 分配三个描述符（无需连续）。
// 磁盘传输总是使用三个描述符。
static int
alloc3_desc(int *idx)
{
  for(int i = 0; i < 3; i++){
    idx[i] = alloc_desc();
    if(idx[i] < 0){
      for(int j = 0; j < i; j++)
        free_desc(idx[j]);
      return -1;
    }
  }
  return 0;
}

void
virtio_disk_rw(struct buf *b, int write)
{
  uint64 sector = b->blockno * (BSIZE / 512);

  acquire(&disk.vdisk_lock);

  // 规范的 5.2 节说明传统块操作使用
  // 三个描述符：一个用于类型/保留字段/扇区，一个用于
  // 数据，一个用于 1 字节的状态结果。

  // 分配三个描述符。
  int idx[3];
  while(1){
    if(alloc3_desc(idx) == 0) {
      break;
    }
    sleep(&disk.free[0], &disk.vdisk_lock);
  }

  // 格式化三个描述符。
  // qemu 的 virtio-blk.c 会读取它们。

  struct virtio_blk_req *buf0 = &disk.ops[idx[0]];

  if(write)
    buf0->type = VIRTIO_BLK_T_OUT; // 写入磁盘
  else
    buf0->type = VIRTIO_BLK_T_IN; // 读取磁盘
  buf0->reserved = 0;
  buf0->sector = sector;

  disk.desc[idx[0]].addr = (uint64) buf0;
  disk.desc[idx[0]].len = sizeof(struct virtio_blk_req);
  disk.desc[idx[0]].flags = VRING_DESC_F_NEXT;
  disk.desc[idx[0]].next = idx[1];

  disk.desc[idx[1]].addr = (uint64) b->data;
  disk.desc[idx[1]].len = BSIZE;
  if(write)
    disk.desc[idx[1]].flags = 0; // 设备读取 b->data
  else
    disk.desc[idx[1]].flags = VRING_DESC_F_WRITE; // 设备写入 b->data
  disk.desc[idx[1]].flags |= VRING_DESC_F_NEXT;
  disk.desc[idx[1]].next = idx[2];

  disk.info[idx[0]].status = 0xff; // 设备在成功时写入 0
  disk.desc[idx[2]].addr = (uint64) &disk.info[idx[0]].status;
  disk.desc[idx[2]].len = 1;
  disk.desc[idx[2]].flags = VRING_DESC_F_WRITE; // 设备写入状态
  disk.desc[idx[2]].next = 0;

  // 为 virtio_disk_intr() 记录 struct buf。
  b->disk = 1;
  disk.info[idx[0]].b = b;

  // 告知设备我们描述符链中的第一个索引。
  disk.avail->ring[disk.avail->idx % NUM] = idx[0];

  __sync_synchronize();

  // 告知设备有另一个可用环条目可用。
  disk.avail->idx += 1; // 不是 % NUM ...

  __sync_synchronize();

  *R(VIRTIO_MMIO_QUEUE_NOTIFY) = 0; // 值为队列编号

  // 等待 virtio_disk_intr() 通知请求已完成。
  while(b->disk == 1) {
    sleep(b, &disk.vdisk_lock);
  }

  disk.info[idx[0]].b = 0;
  free_chain(idx[0]);

  release(&disk.vdisk_lock);
}

void
virtio_disk_intr()
{
  acquire(&disk.vdisk_lock);

  // 在我们告知设备已看到此中断之前，设备不会
  // 再次触发中断，下一行代码即完成此任务。
  // 这可能与设备向"used"环写入新条目
  // 产生竞争，在这种情况下，我们可能在此中断中
  // 处理新的完成条目，而下一个中断无事可做，
  // 这并无危害。
  *R(VIRTIO_MMIO_INTERRUPT_ACK) = *R(VIRTIO_MMIO_INTERRUPT_STATUS) & 0x3;

  __sync_synchronize();

  // 设备在将条目添加到 used 环时
  // 递增 disk.used->idx。

  while(disk.used_idx != disk.used->idx){
    __sync_synchronize();
    int id = disk.used->ring[disk.used_idx % NUM].id;

    if(disk.info[id].status != 0)
      panic("virtio_disk_intr status");

    struct buf *b = disk.info[id].b;
    b->disk = 0;   // 磁盘已完成对 buf 的操作
    wakeup(b);

    disk.used_idx += 1;
  }

  release(&disk.vdisk_lock);
}
