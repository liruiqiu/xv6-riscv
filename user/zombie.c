// 创建一个僵尸进程，它
// 必须在退出时被重新分配父进程。

#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(void)
{
  if(fork() > 0)
    pause(5);  // 让子进程在父进程之前退出。
  exit(0);
}
