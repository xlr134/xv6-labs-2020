#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct spinlock tickslock;
uint ticks;

extern char trampoline[], uservec[], userret[];

// in kernelvec.S, calls kerneltrap().
void kernelvec();

extern int devintr();

void
trapinit(void)
{
  initlock(&tickslock, "time");
}

// set up to take exceptions and traps while in the kernel.
void
trapinithart(void)
{
  w_stvec((uint64)kernelvec);
}

//
// handle an interrupt, exception, or system call from user space.
// called from trampoline.S
//
void
usertrap(void)
{
  int which_dev = 0;

  if((r_sstatus() & SSTATUS_SPP) != 0)  // 判断是否来自于用户态
    panic("usertrap: not from user mode");

  // send interrupts and exceptions to kerneltrap(),
  // since we're now in the kernel.
  // 由于我们现在处于内核中，将中断和异常发送至 kerneltrap () 函数处理
  w_stvec((uint64)kernelvec);//这行代码的作用是将 RISC-V 处理器的异常向量基地址寄存器（stvec） 设置为 kernelvec 标签的地址

  struct proc *p = myproc();
  
  // save user program counter.
  p->trapframe->epc = r_sepc();
  
  if(r_scause() == 8){
    // system call

    if(p->killed)
      exit(-1);

    // sepc points to the ecall instruction,
    // but we want to return to the next instruction.
    p->trapframe->epc += 4;//跳过ecall指令，执行下一条指令

    // an interrupt will change sstatus &c registers,
    // so don't enable until done with those registers.
    intr_on();// 在处理系统调用前启用中断，（允许嵌套中断）

    syscall();//根据 a7 寄存器的值调用对应的系统调用服务例程
  } else if((which_dev = devintr()) != 0){
        // ok
  } else {
    printf("usertrap(): unexpected scause %p pid=%d\n", r_scause(), p->pid);
    printf("            sepc=%p stval=%p\n", r_sepc(), r_stval());
    p->killed = 1;
  }

  if(p->killed)
    exit(-1);

  // give up the CPU if this is a timer interrupt.
  if(which_dev == 2){
    if(p->alarm_interval!=0&&--p->alarm_ticks<=0&&p->alarm_goingoff==0){
      // 是否设置了时钟 && 时钟倒计时是否结束 && 没有其他时钟正在运行
      // 如果一个时钟到期的时候已经有一个时钟处理函数正在运行，
      // 则会推迟到原处理函数运行完成后的下一个 tick 才触发这次时钟
      p->alarm_ticks = p->alarm_interval;
      *(p->alarm_trapframe) = *(p->trapframe);
      p->trapframe->epc = (uint64)p->alarm_handler;
      p->alarm_goingoff = 1;
    }
    yield();//若为定时器中断，让出CPU（进程调度）
  }

  usertrapret();
}

//
// return to user space
//
void
usertrapret(void)
{
  struct proc *p = myproc();

  // we're about to switch the destination of traps from
  // kerneltrap() to usertrap(), so turn off interrupts until
  // we're back in user space, where usertrap() is correct.
  intr_off();// 禁用中断，确保在模式切换过程中不会被中断干扰   这里会切换到用户模式

  // send syscalls, interrupts, and exceptions to trampoline.S
  //设置陷阱向量表地址，将未来的陷阱处理重定向到用户空间的处理函数
  w_stvec(TRAMPOLINE + (uservec - trampoline));

  // set up trapframe values that uservec will need when
  // the process next re-enters the kernel.
  //配置陷阱帧(trapframe)中的值，这些值将在进程下次进入内核时被使用
  p->trapframe->kernel_satp = r_satp();         // kernel page table
  p->trapframe->kernel_sp = p->kstack + PGSIZE; // process's kernel stack
  p->trapframe->kernel_trap = (uint64)usertrap;
  p->trapframe->kernel_hartid = r_tp();         // hartid for cpuid()

  // set up the registers that trampoline.S's sret will use
  // to get to user space.
  
  // set S Previous Privilege mode to User.
  // 修改SSTATUS寄存器：
  // 1. 清除SPP位(Supervisor Previous Privilege)，设置为用户模式
  // 2. 设置SPIE位(Supervisor Previous Interrupt Enable)，启用用户模式中断
  unsigned long x = r_sstatus();
  x &= ~SSTATUS_SPP; // clear SPP to 0 for user mode
  x |= SSTATUS_SPIE; // enable interrupts in user mode
  w_sstatus(x);

  // set S Exception Program Counter to the saved user pc.
  //设置SEPC寄存器为之前保存的用户程序计数器(PC)，决定返回后从哪里继续执行
  w_sepc(p->trapframe->epc);

  // tell trampoline.S the user page table to switch to. 准备用户页表的SATP值，用于在切换到用户模式时切换地址空间
  uint64 satp = MAKE_SATP(p->pagetable);

  // jump to trampoline.S at the top of memory, which 
  // switches to the user page table, restores user registers,
  // and switches to user mode with sret.
  // 跳转到trampoline区域的userret函数，执行实际的模式切换
  // 传递陷阱帧地址和用户页表的SATP值作为参数
  uint64 fn = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64,uint64))fn)(TRAPFRAME, satp);
}

// interrupts and exceptions from kernel code go here via kernelvec,
// on whatever the current kernel stack is.
// 内核代码产生的中断和异常会通过 kernelvec 路由到此处处理，
// 此时使用的是当前内核栈（无论其为何种状态）
void 
kerneltrap()
{
  int which_dev = 0;
  uint64 sepc = r_sepc();// 获取异常发生时的PC值
  uint64 sstatus = r_sstatus();//获取当前CPU状态（如中断使能标志）
  uint64 scause = r_scause();//获取异常原因（同步/异步、具体类型）
 
  if((sstatus & SSTATUS_SPP) == 0)// 验证上下文确保中断/异常来自正确的特权模式
    panic("kerneltrap: not from supervisor mode");
  if(intr_get() != 0)//intr_get()：检查当前是否允许中断（正常情况下，中断处理期间应禁用中断）。
    panic("kerneltrap: interrupts enabled");

  if((which_dev = devintr()) == 0){
    printf("scause %p\n", scause);
    printf("sepc=%p stval=%p\n", r_sepc(), r_stval());
    panic("kerneltrap");
  }

  // give up the CPU if this is a timer interrupt.
  if(which_dev == 2 && myproc() != 0 && myproc()->state == RUNNING)
    yield(); //会执行调度器确定要执行的下一个进程

  // the yield() may have caused some traps to occur,
  // so restore trap registers for use by kernelvec.S's sepc instruction.
  w_sepc(sepc);
  w_sstatus(sstatus);
}

void
clockintr()
{
  acquire(&tickslock);
  ticks++;
  wakeup(&ticks);
  release(&tickslock);
}

// check if it's an external interrupt or software interrupt,
// and handle it.
// returns 2 if timer interrupt,
// 1 if other device,
// 0 if not recognized.
// 检查是外部中断还是软件中断，
// 并对其进行处理。
// 若为定时器中断则返回 2，
// 若为其他设备中断则返回 1，
// 若未识别则返回 0。
int
devintr()
{
  uint64 scause = r_scause();
  //“scause” 是 RISC-V 架构中的一个专用术语，通常译为 “超级用户模式异常原因寄存器”（Supervisor Cause Register）
  //或 “超级用户模式异常原因”（Supervisor Cause）

  if((scause & 0x8000000000000000L) &&
     (scause & 0xff) == 9){
    // this is a supervisor external interrupt, via PLIC.

    // irq indicates which device interrupted.
    // 这是一个通过 PLIC（平台级中断控制器）触发的超级用户模式外部中断。
    // irq 用于指示是哪个设备产生的中断。
    int irq = plic_claim();

    if(irq == UART0_IRQ){
      uartintr();
    } else if(irq == VIRTIO0_IRQ){
      virtio_disk_intr();
    } else if(irq){
      printf("unexpected interrupt irq=%d\n", irq);
    }

    // the PLIC allows each device to raise at most one
    // interrupt at a time; tell the PLIC the device is
    // now allowed to interrupt again.
    // PLIC 允许每个设备一次最多只能发起一个中断；告知 PLIC 现在允许该设备再次发起中断。
    if(irq)
      plic_complete(irq);

    return 1;
  } else if(scause == 0x8000000000000001L){
    // software interrupt from a machine-mode timer interrupt,
    // forwarded by timervec in kernelvec.S.
    // 软件中断，源自机器模式(M-mode)的定时器中断，
    // 由 kernelvec.S 中的 timervec 转发而来。
    if(cpuid() == 0){//条件执行时钟中断处理  在多核系统中，只有一个核心负责时钟维护，避免多核心同时修改时钟导致的数据竞争。
      clockintr();
    }
    
    // acknowledge the software interrupt by clearing
    // the SSIP bit in sip.
    w_sip(r_sip() & ~2);//清除软件中断标志位

    return 2;
  } else {
    return 0;
  }
}


// 设置进程中时钟相关的属性

int
_sigalarm(int ticks, void (*handler)(void)){
  struct proc *p = myproc();
  p->alarm_interval = ticks;
  p->alarm_handler = handler;
  p->alarm_ticks = ticks;
  return 0;
}

int 
_sigreturn(void){
  struct proc *p = myproc();
  *(p->trapframe)= *(p->alarm_trapframe);
  p->alarm_goingoff = 0;
  return 0;
}