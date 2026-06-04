// 磁盘上的文件系统格式。
// 内核和用户程序都使用此头文件。


#define ROOTINO  1   // 根 inode 编号
#define BSIZE 1024  // 块大小

// 磁盘布局：
// [ 引导块 | 超级块 | 日志 | inode 块 |
//                            空闲位图 | 数据块 ]
//
// mkfs 计算超级块并构建初始文件系统。
// 超级块描述磁盘布局：
struct superblock {
  uint magic;        // 必须为 FSMAGIC
  uint size;         // 文件系统映像大小（块）
  uint nblocks;      // 数据块数量
  uint ninodes;      // inode 数量
  uint nlog;         // 日志块数量
  uint logstart;     // 第一个日志块的块号
  uint inodestart;   // 第一个 inode 块的块号
  uint bmapstart;    // 第一个空闲位图块的块号
};

#define FSMAGIC 0x10203040

#define NDIRECT 12
#define NINDIRECT (BSIZE / sizeof(uint))
#define MAXFILE (NDIRECT + NINDIRECT)

// 磁盘上的 inode 结构
struct dinode {
  short type;           // 文件类型
  short major;          // 主设备号（仅 T_DEVICE）
  short minor;          // 次设备号（仅 T_DEVICE）
  short nlink;          // 文件系统中指向此 inode 的链接数
  uint size;            // 文件大小（字节）
  uint addrs[NDIRECT+1];   // 数据块地址
};

// 每个块的 inode 数。
#define IPB           (BSIZE / sizeof(struct dinode))

// 包含 inode i 的块
#define IBLOCK(i, sb)     ((i) / IPB + sb.inodestart)

// 每个块的位图位数
#define BPB           (BSIZE*8)

// 包含块 b 对应位的空闲位图块
#define BBLOCK(b, sb) ((b)/BPB + sb.bmapstart)

// 目录是一个包含一系列 dirent 结构的文件。
#define DIRSIZ 14

// name 字段最多可有 DIRSIZ 个字符，不一定以 NUL 字符
// 结尾。
struct dirent {
  ushort inum;
  char name[DIRSIZ] __attribute__((nonstring));
};

