//
// 16550a UART 底层驱动程序。
//

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

// UART 控制寄存器是内存映射的，
// 位于地址 UART0。此宏返回
// 某个寄存器的地址。
#define Reg(reg) ((volatile unsigned char *)(UART0 + (reg)))

#define ReadReg(reg) (*(Reg(reg)))
#define WriteReg(reg, v) (*(Reg(reg)) = (v))

// UART 控制寄存器。
// 某些寄存器在读取和写入时的含义不同。
// 参见 http://byterunner.com/16550.html
#define RHR 0                 // 接收保持寄存器（用于输入字节）
#define THR 0                 // 发送保持寄存器（用于输出字节）
#define IER 1                 // 中断使能寄存器
#define IER_RX_ENABLE (1<<0)
#define IER_TX_ENABLE (1<<1)
#define FCR 2                 // FIFO 控制寄存器
#define FCR_FIFO_ENABLE (1<<0)
#define FCR_FIFO_CLEAR (3<<1) // 清除两个 FIFO 的内容
#define ISR 2                 // 中断状态寄存器
#define LCR 3                 // 线路控制寄存器
#define LCR_EIGHT_BITS (3<<0)
#define LCR_BAUD_LATCH (1<<7) // 设置波特率的特殊模式
#define LSR 5                 // 线路状态寄存器
#define LSR_RX_READY (1<<0)   // RHR 中有待读取的输入
#define LSR_TX_IDLE (1<<5)    // THR 可以接受下一个待发送字符

// 用于发送线程与 UART "就绪"中断同步。
static struct spinlock tx_lock;
static int tx_busy;           // UART 是否正忙于发送？
static int tx_chan;           // &tx_chan 是"等待通道"

extern volatile int panicking; // 来自 printf.c
extern volatile int panicked; // 来自 printf.c

void
uartinit(void)
{
  // 禁用中断。
  WriteReg(IER, 0x00);

  // 设置波特率的特殊模式。
  WriteReg(LCR, LCR_BAUD_LATCH);

  // 38.4K 波特率的 LSB。
  WriteReg(0, 0x03);

  // 38.4K 波特率的 MSB。
  WriteReg(1, 0x00);

  // 退出设置波特率模式，
  // 设置字长为 8 位，无校验。
  WriteReg(LCR, LCR_EIGHT_BITS);

  // 复位并启用 FIFO。
  WriteReg(FCR, FCR_FIFO_ENABLE | FCR_FIFO_CLEAR);

  // 启用发送和接收中断。
  WriteReg(IER, IER_TX_ENABLE | IER_RX_ENABLE);

  initlock(&tx_lock, "uart");
}

// 将 buf[] 发送到 UART。如果 UART 正忙则阻塞，
// 因此不能从中断中调用，
// 只能从 write() 系统调用中调用。
void
uartwrite(char buf[], int n)
{
  acquire(&tx_lock);

  int i = 0;
  while(i < n){ 
    while(tx_busy != 0){
      // 等待 UART 发送完成中断
      // 将 tx_busy 设为 0。
      sleep(&tx_chan, &tx_lock);
    }   
      
    WriteReg(THR, buf[i]);
    i += 1;
    tx_busy = 1;
  }

  release(&tx_lock);
}


// 向 UART 写入一个字节，不使用中断，
// 供内核 printf() 和字符回显使用。
// 它自旋等待 UART 输出寄存器
// 变为空闲。
void
uartputc_sync(int c)
{
  if(panicking == 0)
    push_off();

  if(panicked){
    for(;;)
      ;
  }

  // 等待 UART 将 LSR 中的 Transmit Holding Empty 置 1。
  while((ReadReg(LSR) & LSR_TX_IDLE) == 0)
    ;
  WriteReg(THR, c);

  if(panicking == 0)
    pop_off();
}

// 尝试从 UART 读取一个输入字符。
// 如果没有等待的字符，返回 -1。
int
uartgetc(void)
{
  if(ReadReg(LSR) & LSR_RX_READY){
    // 输入数据已就绪。
    return ReadReg(RHR);
  } else {
    return -1;
  }
}

// 处理 UART 中断，因输入到达
// 或 UART 准备好接收更多输出
// 或两者兼有而触发。从 devintr() 调用。
void
uartintr(void)
{
  ReadReg(ISR); // 确认中断

  acquire(&tx_lock);
  if(ReadReg(LSR) & LSR_TX_IDLE){
    // UART 发送完成；唤醒发送线程。
    tx_busy = 0;
    wakeup(&tx_chan);
  }
  release(&tx_lock);

  // 读取并处理到达的字符（如果有）。
  while(1){
    int c = uartgetc();
    if(c == -1)
      break;
    consoleintr(c);
  }
}
