#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "elf.h"

static int loadseg(pde_t *pgdir, uint64 addr, struct inode *ip, uint offset, uint sz);

int
exec(char *path, char **argv)
{
//`exec` 是创建一个地址空间的用户部分的系统调用。它读取储存在文件系统上的文件用来初始化一个地址空间的用户部分。
//`exec`（`kernel/exec.c:13`）使用 `namei`（`kernel/exec.c:26`）打开二进制文件路径
  char *s, *last;
  int i, off;
  uint64 argc, sz = 0, sp, ustack[MAXARG+1], stackbase;
  struct elfhdr elf;
  struct inode *ip;
  struct proghdr ph;
  pagetable_t pagetable = 0, oldpagetable;
  struct proc *p = myproc();

  begin_op();

  if((ip = namei(path)) == 0){// 打开二进制文件 xv6以ELF的格式描述可执行文件
    end_op();
    return -1;
  }
  ilock(ip);

  // Check ELF header
  if(readi(ip, 0, (uint64)&elf, 0, sizeof(elf)) != sizeof(elf))
    goto bad;
  if(elf.magic != ELF_MAGIC)
    goto bad;

  if((pagetable = proc_pagetable(p)) == 0)// 为当前进程创建页表
    goto bad;

  // Load program into memory.
  for(i=0, off=elf.phoff; i<elf.phnum; i++, off+=sizeof(ph)){
    if(readi(ip, 0, (uint64)&ph, off, sizeof(ph)) != sizeof(ph))
      goto bad;
    if(ph.type != ELF_PROG_LOAD)// 只处理需要加载到内存中的段
      continue;
    if(ph.memsz < ph.filesz)
      goto bad;
    if(ph.vaddr + ph.memsz < ph.vaddr)// 防止整数溢出
      goto bad;
    uint64 sz1;
    if((sz1 = uvmalloc(pagetable, sz, ph.vaddr + ph.memsz)) == 0)// 注意这里是ph.vaddr+ph.memsz
      goto bad;
    if(sz1>=PLIC) // 为了在内核态也能使用用户页表
      goto bad;
    sz = sz1;
    if(ph.vaddr % PGSIZE != 0)
      goto bad;
    if(loadseg(pagetable, ph.vaddr, ip, ph.off, ph.filesz) < 0)//从文件加载段内容
      goto bad;
  }
  iunlockput(ip);
  end_op();
  ip = 0;

  p = myproc();
  uint64 oldsz = p->sz;

  // Allocate two pages at the next page boundary.
  // Use the second as the user stack.
  // 在接下来的页边界处分配两个页面。
// 将第二个页面用作用户栈。
  sz = PGROUNDUP(sz);
  uint64 sz1;
  if((sz1 = uvmalloc(pagetable, sz, sz + 2*PGSIZE)) == 0)
    goto bad;
  sz = sz1;
  uvmclear(pagetable, sz-2*PGSIZE);// 将第二个页面设置为保护页 设置为非用户页面
  sp = sz;
  stackbase = sp - PGSIZE;

  // Push argument strings, prepare rest of stack in ustack.
  /*
  将用户程序的命令行参数（字符串数组）复制到用户栈空间
  确保每个参数地址按 16 字节对齐（RISC-V 架构要求）
  构建一个指向这些参数的指针数组（即用户栈上的 argv 数组）
  最终在指针数组末尾添加一个 NULL 指针（标记结束）
  */
  for(argc = 0; argv[argc]; argc++) {
    if(argc >= MAXARG)
      goto bad;
    sp -= strlen(argv[argc]) + 1;
    sp -= sp % 16; // riscv sp must be 16-byte aligned
    if(sp < stackbase)
      goto bad;
    if(copyout(pagetable, sp, argv[argc], strlen(argv[argc]) + 1) < 0)
      goto bad;
    ustack[argc] = sp;
  }
  ustack[argc] = 0;

  // push the array of argv[] pointers. 这段代码是操作系统将命令行参数指针数组（argv）写入用户栈的关键步骤，完成了从内核临时数组到用户栈的最终复制。我来详细解释：
  sp -= (argc+1) * sizeof(uint64);
  sp -= sp % 16;
  if(sp < stackbase)
    goto bad;
  if(copyout(pagetable, sp, (char *)ustack, (argc+1)*sizeof(uint64)) < 0)// 传入参数所在的栈地址
    goto bad;

  // arguments to user main(argc, argv)
  // argc is returned via the system call return
  // value, which goes in a0.
// 用户main(argc, argv)函数的参数
// argc通过系统调用的返回值传递
// 该返回值会被放入a0寄存器中。
  p->trapframe->a1 = sp;

  // Save program name for debugging. //保存程序名称以进行调试
  for(last=s=path; *s; s++)
    if(*s == '/')
      last = s+1;
  safestrcpy(p->name, last, sizeof(p->name));
  
  //清除内核页表中对程序内存的旧映射 然后重新建立映射
  uvmunmap(p->kernelpgtbl,0,PGROUNDUP(oldsz)/PGSIZE,0);
  kvmcopymappings(pagetable,p->kernelpgtbl,0,sz);

  // Commit to the user image.
  oldpagetable = p->pagetable; 
  p->pagetable = pagetable;     //切换进程的页表（虚拟内存映射）
  p->sz = sz;                  //更新进程的内存大小
  p->trapframe->epc = elf.entry;  // initial program counter = main 设置程序入口点（初始 PC 寄存器）
  p->trapframe->sp = sp; // initial stack pointer 设置初始栈指针（初始 SP 寄存器）
  proc_freepagetable(oldpagetable, oldsz);
  /*
  切换页表：将进程的地址空间更新为新程序的页表。
  设置执行环境：配置程序入口点（main函数地址）和初始栈指针。
  释放旧资源：回收旧程序的页表和内存空间。
  特殊处理：对 PID=1（init 进程）打印页表信息用于调试。
  返回参数数量：将argc作为系统调用返回值传递给用户程序。
*/
  //PID=1：在 UNIX 系统中，PID=1 的进程是init进程，它是系统启动后创建的第一个用户进程，负责初始化系统环境、启动守护进程等  这是打印页表的代码
  // if(p->pid==1)   
  //   vmprint(p->pagetable);

  return argc; // this ends up in a0, the first argument to main(argc, argv)

 bad:
  if(pagetable)
    proc_freepagetable(pagetable, sz);
  if(ip){
    iunlockput(ip);
    end_op();
  }
  return -1;
}

// Load a program segment into pagetable at virtual address va.
// va must be page-aligned
// and the pages from va to va+sz must already be mapped.
// Returns 0 on success, -1 on failure.
static int
loadseg(pagetable_t pagetable, uint64 va, struct inode *ip, uint offset, uint sz)
{
  uint i, n;
  uint64 pa;

  if((va % PGSIZE) != 0)
    panic("loadseg: va must be page aligned");

  for(i = 0; i < sz; i += PGSIZE){
    pa = walkaddr(pagetable, va + i);
    if(pa == 0)
      panic("loadseg: address should exist");
    if(sz - i < PGSIZE)
      n = sz - i;
    else
      n = PGSIZE;
    if(readi(ip, 0, (uint64)pa, offset+i, n) != n)
      return -1;
  }
  
  return 0;
}
