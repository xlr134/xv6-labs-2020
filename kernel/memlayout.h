// Physical memory layout

// qemu -machine virt is set up like this,
// based on qemu's hw/riscv/virt.c:
//
// 00001000 -- boot ROM, provided by qemu
// 02000000 -- CLINT
// 0C000000 -- PLIC
// 10000000 -- uart0 
// 10001000 -- virtio disk 
// 80000000 -- boot ROM jumps here in machine mode
//             -kernel loads the kernel here
// unused RAM after 80000000.

// the kernel uses physical memory thus:
// 80000000 -- entry.S, then kernel text and data
// end -- start of kernel page allocation area
// PHYSTOP -- end RAM used by the kernel

// qemu puts UART registers here in physical memory.

// 物理内存布局
//qemu -machine virt 机器的内存布局如下（基于 qemu 的 hw/riscv/virt.c 文件）：
//
// 00001000 -- 引导 ROM（由 qemu 提供）
// 02000000 -- CLINT（通用定时器与中断控制器）
// 0C000000 -- PLIC（平台级中断控制器）
// 10000000 -- uart0（串口设备）
// 10001000 -- virtio disk（虚拟磁盘设备）
// 80000000 -- 引导 ROM 在机器模式下跳转到此处
// -kernel 会将内核加载到该地址
// 80000000 之后为未使用的 RAM

// 内核对物理内存的使用方式如下：
// 80000000 -- 存放 entry.S，之后是内核文本段和数据段
//end -- 内核页分配区域的起始地址
// PHYSTOP -- 内核使用的 RAM 结束地址

//qemu 将 UART 寄存器映射到物理内存的此处。

#define UART0 0x10000000L
#define UART0_IRQ 10

// virtio mmio interface  virtio 内存映射输入输出接口
#define VIRTIO0 0x10001000
#define VIRTIO0_IRQ 1

// local interrupt controller, which contains the timer.  本地中断控制器，其中包含定时器。
#define CLINT 0x2000000L
#define CLINT_MTIMECMP(hartid) (CLINT + 0x4000 + 8*(hartid))
#define CLINT_MTIME (CLINT + 0xBFF8) // cycles since boot.

// qemu puts programmable interrupt controller here. qemu 把可编程中断控制器放在这里。
#define PLIC 0x0c000000L
#define PLIC_PRIORITY (PLIC + 0x0)
#define PLIC_PENDING (PLIC + 0x1000)
#define PLIC_MENABLE(hart) (PLIC + 0x2000 + (hart)*0x100)
#define PLIC_SENABLE(hart) (PLIC + 0x2080 + (hart)*0x100)
#define PLIC_MPRIORITY(hart) (PLIC + 0x200000 + (hart)*0x2000)
#define PLIC_SPRIORITY(hart) (PLIC + 0x201000 + (hart)*0x2000)
#define PLIC_MCLAIM(hart) (PLIC + 0x200004 + (hart)*0x2000)
#define PLIC_SCLAIM(hart) (PLIC + 0x201004 + (hart)*0x2000)

// the kernel expects there to be RAM
// for use by the kernel and user pages
// from physical address 0x80000000 to PHYSTOP.

// 内核期望从物理地址 0x80000000 到 PHYSTOP 之间存在供内核和用户页使用的 RAM。
#define KERNBASE 0x80000000L
#define PHYSTOP (KERNBASE + 128*1024*1024)

// map the trampoline page to the highest address,
// in both user and kernel space.
// 将 trampoline 页映射到最高地址，
// 同时存在于用户空间和内核空间中。

#define TRAMPOLINE (MAXVA - PGSIZE)

// map kernel stacks beneath the trampoline,
// each surrounded by invalid guard pages.
// 在内核跳板页（trampoline page）下方映射内核栈，
// 每个内核栈周围用无效的保护页（guard pages）包围。
#define KSTACK(p) (TRAMPOLINE - ((p)+1)* 2*PGSIZE)

// User memory layout.
// Address zero first:
//   text
//   original data and bss
//   fixed-size stack
//   expandable heap
//   ...
//   TRAPFRAME (p->trapframe, used by the trampoline)
//   TRAMPOLINE (the same page as in the kernel)

// 用户内存布局
// 地址从0开始依次为：
//   代码段（text）
//   初始数据段和BSS段
//   固定大小的栈
//   可扩展的堆
//   ...
//   TRAPFRAME（进程的陷阱帧p->trapframe，供跳板页使用）
//   TRAMPOLINE（与内核共享的跳板页）
#define TRAPFRAME (TRAMPOLINE - PGSIZE)