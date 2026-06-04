#define NPROC        64  // 最大进程数
#define NCPU          8  // 最大 CPU 数
#define NOFILE       16  // 每进程可打开的文件数
#define NFILE       100  // 系统可打开的文件总数
#define NINODE       50  // 最大活跃 inode 数
#define NDEV         10  // 最大主设备号
#define ROOTDEV       1  // 文件系统根磁盘的设备号
#define MAXARG       32  // 最大 exec 参数
#define MAXOPBLOCKS  10  // 任何 FS 操作可写入的最大块数
#define LOGBLOCKS    (MAXOPBLOCKS*3)  // 磁盘日志中的最大数据块数
#define NBUF         (MAXOPBLOCKS*3)  // 磁盘块缓存大小
#define FSSIZE       2000  // 文件系统大小（块）
#define MAXPATH      128   // 最大文件路径名
#define USERSTACK    1     // 用户栈页数

