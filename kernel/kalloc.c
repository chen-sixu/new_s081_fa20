// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem[NCPU];

void
kinit()
{
  initlock(&kmem[0].lock, "kmem_0");
  initlock(&kmem[1].lock, "kmem_1");
  initlock(&kmem[2].lock, "kmem_2");
  initlock(&kmem[3].lock, "kmem_3");
  initlock(&kmem[4].lock, "kmem_4");
  initlock(&kmem[5].lock, "kmem_5");
  initlock(&kmem[6].lock, "kmem_6");
  initlock(&kmem[7].lock, "kmem_7");
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  push_off();
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;
  int cpu_id = cpuid();
  acquire(&kmem[cpu_id].lock);
  r->next = kmem[cpu_id].freelist;
  kmem[cpu_id].freelist = r;
  release(&kmem[cpu_id].lock);
  pop_off();
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;
  push_off();
  int cpu_id = cpuid();
  acquire(&kmem[cpu_id].lock);
  r = kmem[cpu_id].freelist;
  if(r) { // kmem[cpu_id].freelist不为空
    kmem[cpu_id].freelist = r->next; // 把r添加到freelist头部
  }
  release(&kmem[cpu_id].lock);

  if(r == 0) {
    // 当kmem[cpu_id].freelist为空时，需要从其他cpu偷内存
    for(int i = 0; i < NCPU; i++) {
      if(i == cpu_id) continue;
      acquire(&kmem[i].lock);
	  r = kmem[i].freelist;
	  if(r){
	    kmem[i].freelist = r->next;
	  }
    release(&kmem[i].lock); // 偷到了i的内存，释放锁
    if(r) break;
	/*
	如果没有这个break，循环永远会走到底，r会一直是cpu[7]的头节点。
	make qemu 时，cpu[7]的freelist可能是空的，（物理内存全都在第一次运行freearange的cpu那里，而第一次运行的不一定是cpu[7]）
	那么剩下的cpu的freelist都为空，也就是只有一个cpu能正常运行
	这里会发生的报错是：panic:init exiting，也就是说init进程自己退出了。
	根据backtrace(此处省略)

	可能的原因：user/initcode.S:
	1. 加载 /init 字符串的地址到寄存器 a0，加载 argv 数组的地址到寄存器 a1。
	2. 调用 exec 系统调用，执行 /init 程序。
	3. 如果 exec 系统调用成功（/init 启动），它将替代当前进程，进入新的进程空间。
	4. 如果执行失败，代码进入 exit 标签，进入无限循环并不断调用 exit 系统调用终止当前进程。

	第一个cpu有属于他的物理内存，正常运行init进程，也就是0号进程
	第二个cpu没有属于自己的物理内存，执行失败
	第一个cpu上的init进程调用exit，此后就是backtrace中的函数调用栈，导致panic:init exiting
	*/
    }
  }

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  pop_off();
  return (void*)r;
}

/* 
对每个cpu分配一个freelist，一个对应的lock。在初始化的时候，先将所有空页给第一个cpu
后续的每个cpu第一次使用时，肯定都会偷一次，因为freelist为空，这个过程会有一个锁的竞争

原先：所有cpu共用物理内存，想要kalloc，就要acquire锁
现在：每个cpu有自己的freelist，需要内存时先使用自己的freelist中的物理内存，如果够用，则直接使用，减少了锁的竞争
*/