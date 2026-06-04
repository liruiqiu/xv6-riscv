//
// virtio 设备定义。
// 包括 mmio 接口和 virtio 描述符。
// 仅在 qemu 中测试过。
//
// virtio 规范：
// https://docs.oasis-open.org/virtio/virtio/v1.1/virtio-v1.1.pdf
//

// virtio mmio 控制寄存器，映射起始地址为 0x10001000。
// 来自 qemu virtio_mmio.h
#define VIRTIO_MMIO_MAGIC_VALUE		0x000 // 0x74726976
#define VIRTIO_MMIO_VERSION		0x004 // 版本；应为 2
#define VIRTIO_MMIO_DEVICE_ID		0x008 // 设备类型；1 为网络，2 为磁盘
#define VIRTIO_MMIO_VENDOR_ID		0x00c // 0x554d4551
#define VIRTIO_MMIO_DEVICE_FEATURES	0x010
#define VIRTIO_MMIO_DRIVER_FEATURES	0x020
#define VIRTIO_MMIO_QUEUE_SEL		0x030 // 选择队列，只写
#define VIRTIO_MMIO_QUEUE_NUM_MAX	0x034 // 当前队列最大大小，只读
#define VIRTIO_MMIO_QUEUE_NUM		0x038 // 当前队列大小，只写
#define VIRTIO_MMIO_QUEUE_READY		0x044 // 就绪位
#define VIRTIO_MMIO_QUEUE_NOTIFY	0x050 // 只写
#define VIRTIO_MMIO_INTERRUPT_STATUS	0x060 // 只读
#define VIRTIO_MMIO_INTERRUPT_ACK	0x064 // 只写
#define VIRTIO_MMIO_STATUS		0x070 // 读/写
#define VIRTIO_MMIO_QUEUE_DESC_LOW	0x080 // 描述符表的物理地址，只写
#define VIRTIO_MMIO_QUEUE_DESC_HIGH	0x084
#define VIRTIO_MMIO_DRIVER_DESC_LOW	0x090 // 可用环的物理地址，只写
#define VIRTIO_MMIO_DRIVER_DESC_HIGH	0x094
#define VIRTIO_MMIO_DEVICE_DESC_LOW	0x0a0 // 已用环的物理地址，只写
#define VIRTIO_MMIO_DEVICE_DESC_HIGH	0x0a4

// 状态寄存器位，来自 qemu virtio_config.h
#define VIRTIO_CONFIG_S_ACKNOWLEDGE	1
#define VIRTIO_CONFIG_S_DRIVER		2
#define VIRTIO_CONFIG_S_DRIVER_OK	4
#define VIRTIO_CONFIG_S_FEATURES_OK	8

// 设备特性位
#define VIRTIO_BLK_F_RO              5	/* 磁盘为只读 */
#define VIRTIO_BLK_F_SCSI            7	/* 支持 SCSI 命令透传 */
#define VIRTIO_BLK_F_CONFIG_WCE     11	/* 配置中提供写回模式 */
#define VIRTIO_BLK_F_MQ             12	/* 支持多个 virtqueue */
#define VIRTIO_F_ANY_LAYOUT         27
#define VIRTIO_RING_F_INDIRECT_DESC 28
#define VIRTIO_RING_F_EVENT_IDX     29

// 这么多 virtio 描述符。
// 必须是 2 的幂。
#define NUM 8

// 单个描述符，来自规范。
struct virtq_desc {
  uint64 addr;
  uint32 len;
  uint16 flags;
  uint16 next;
};
#define VRING_DESC_F_NEXT  1 // 与另一个描述符链接
#define VRING_DESC_F_WRITE 2 // 设备写入（相对于读取）

// （整个）可用环，来自规范。
struct virtq_avail {
  uint16 flags; // 始终为零
  uint16 idx;   // 驱动程序下次将写入 ring[idx]
  uint16 ring[NUM]; // 链头的描述符编号
  uint16 unused;
};

// "已用"环中的一项，设备通过它
// 告知驱动程序已完成的请求。
struct virtq_used_elem {
  uint32 id;   // 已完成描述符链的起始索引
  uint32 len;
};

struct virtq_used {
  uint16 flags; // 始终为零
  uint16 idx;   // 设备添加 ring[] 条目时递增
  struct virtq_used_elem ring[NUM];
};

// 以下是 virtio 块设备（如磁盘）的特定定义，
// 参见规范 5.2 节。

#define VIRTIO_BLK_T_IN  0 // 读取磁盘
#define VIRTIO_BLK_T_OUT 1 // 写入磁盘

// 磁盘请求中第一个描述符的格式。
// 其后跟随另外两个描述符，分别包含
// 块和一个一字节的状态。
struct virtio_blk_req {
  uint32 type; // VIRTIO_BLK_T_IN 或 ..._OUT
  uint32 reserved;
  uint64 sector;
};
