//
// 控制台输入和输出，通过 UART。
// 每次读取一行。
// 实现特殊输入字符：
//   newline —— 行结束
//   control-h —— 退格
//   control-u —— 删除整行
//   control-d —— 文件结束
//   control-p —— 打印进程列表
//

#include <stdarg.h>

#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"
#include "proc.h"

#define BACKSPACE 0x100  // 擦除最后一个输出字符
#define C(x)  ((x)-'@')  // Control-x

//
// 向 UART 发送一个字符，但不使用
// 中断或 sleep()。可以安全地从中断中调用，
// 例如被 printf 调用或用于回显输入
// 字符。
//
void
consputc(int c)
{
  if(c == BACKSPACE){
    // 如果用户输入了退格，用空格覆盖。
    uartputc_sync('\b'); uartputc_sync(' '); uartputc_sync('\b');
  } else {
    uartputc_sync(c);
  }
}

struct {
  struct spinlock lock;
  
  // 输入环形缓冲区
#define INPUT_BUF_SIZE 128
  char buf[INPUT_BUF_SIZE];
  uint r;  // 读索引
  uint w;  // 写索引
  uint e;  // 编辑索引
} cons;

//
// 控制台的用户 write() 系统调用在此处理。
// 使用 sleep() 和 UART 中断。
//
int
consolewrite(int user_src, uint64 src, int n)
{
  char buf[32]; // 批量从用户空间移到 UART。
  int i = 0;

  while(i < n){
    int nn = sizeof(buf);
    if(nn > n - i)
      nn = n - i;
    if(either_copyin(buf, user_src, src+i, nn) == -1)
      break;
    uartwrite(buf, nn);
    i += nn;
  }

  return i;
}

//
// 控制台的用户 read() 在此处理。
// 将（最多）一整行输入拷贝到 dst。
// user_dst 指示 dst 是用户地址
// 还是内核地址。
//
int
consoleread(int user_dst, uint64 dst, int n)
{
  uint target;
  int c;
  char cbuf;

  target = n;
  acquire(&cons.lock);
  while(n > 0){
    // 等待中断处理程序将一些
    // 输入放入 cons.buffer。
    while(cons.r == cons.w){
      if(killed(myproc())){
        release(&cons.lock);
        return -1;
      }
      sleep(&cons.r, &cons.lock);
    }

    c = cons.buf[cons.r++ % INPUT_BUF_SIZE];

    if(c == C('D')){  // 文件结束
      if(n < target){
        // 将 ^D 留到下一次，以确保
        // 调用者获得 0 字节的结果。
        cons.r--;
      }
      break;
    }

    // 将输入字节拷贝到用户空间缓冲区。
    cbuf = c;
    if(either_copyout(user_dst, dst, &cbuf, 1) == -1)
      break;

    dst++;
    --n;

    if(c == '\n'){
      // 一整行已到达，返回到
      // 用户级 read()。
      break;
    }
  }
  release(&cons.lock);

  return target - n;
}

//
// 控制台输入中断处理程序。
// uartintr() 对每个输入字符调用此函数。
// 进行擦除/删除处理，追加到 cons.buf，
// 如果一整行到达则唤醒 consoleread()。
//
void
consoleintr(int c)
{
  acquire(&cons.lock);

  switch(c){
  case C('P'):  // 打印进程列表。
    procdump();
    break;
  case C('U'):  // 删除整行。
    while(cons.e != cons.w &&
          cons.buf[(cons.e-1) % INPUT_BUF_SIZE] != '\n'){
      cons.e--;
      consputc(BACKSPACE);
    }
    break;
  case C('H'): // 退格
  case '\x7f': // 删除键
    if(cons.e != cons.w){
      cons.e--;
      consputc(BACKSPACE);
    }
    break;
  default:
    if(c != 0 && cons.e-cons.r < INPUT_BUF_SIZE){
      c = (c == '\r') ? '\n' : c;

      // 回显给用户。
      consputc(c);

      // 存储以供应 consoleread() 消费。
      cons.buf[cons.e++ % INPUT_BUF_SIZE] = c;

      if(c == '\n' || c == C('D') || cons.e-cons.r == INPUT_BUF_SIZE){
        // 如果一整行（或文件结束符）已到达，
        // 唤醒 consoleread()。
        cons.w = cons.e;
        wakeup(&cons.r);
      }
    }
    break;
  }
  
  release(&cons.lock);
}

void
consoleinit(void)
{
  initlock(&cons.lock, "cons");

  uartinit();

  // 将 read 和 write 系统调用
  // 连接到 consoleread 和 consolewrite。
  devsw[CONSOLE].read = consoleread;
  devsw[CONSOLE].write = consolewrite;
}
