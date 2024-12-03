// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

#define NBUCKET 13
#define NB 5

extern uint ticks;

struct {
  struct spinlock lock;
  struct buf buf[NB];

  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
  // struct buf head;
} bcache[NBUCKET];
// 13个桶，每个桶有5个buf

int
hash(int n)
{
  return n % NBUCKET;
}

void
binit(void)
{
  // struct buf *b;
  char temp1[20];
  for (int i = 0; i < NBUCKET; i++) {
	snprintf(temp1, sizeof(temp1), "bcache_%d", i);
    initlock(&bcache[i].lock, temp1);
    char temp2[20];
	for (int j = 0; j < NB; j++) {
	  snprintf(temp2, sizeof(temp2), "sleeplock_%d", j);
	  initsleeplock(&bcache[i].buf[j].lock, temp2);
	}
  }
  


  // Create linked list of buffers
//   bcache.head.prev = &bcache.head;
//   bcache.head.next = &bcache.head;
//   for(b = bcache.buf; b < bcache.buf+NBUF; b++){
//     b->next = bcache.head.next;
//     b->prev = &bcache.head;
//     initsleeplock(&b->lock, "buffer");
//     bcache.head.next->prev = b;
//     bcache.head.next = b;
//   }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;
  int hash_b = hash(blockno);

  acquire(&bcache[hash_b].lock);

  // Is the block already cached?
  for(int i = 0; i < NB; i++){
    b = &bcache[hash_b].buf[i];
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      b->lastuse = ticks;
      release(&bcache[hash_b].lock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // Not cached.
  // Recycle the least recently used (LRU) unused buffer.
  struct buf temp_buf;
  uint min = 0xffffffff;
  int index = -1;
  for(int j = 0; j<NB; j++){
    temp_buf = bcache[hash_b].buf[j];
    if(temp_buf.lastuse < min && temp_buf.refcnt == 0) { //当前这个temp是最近最少使用
      min = temp_buf.lastuse;
	  index = j;
    }
  }
  if (index < 0)
	panic("bget: no buffers");
  b = &bcache[hash_b].buf[index];
  b->dev = dev;
  b->blockno = blockno;
  b->valid = 0;
  b->refcnt = 1;
  release(&bcache[hash_b].lock);
  acquiresleep(&b->lock);
  return b;
  
  panic("bget: no buffers");
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  int hash_b = hash(b->blockno);

  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  acquire(&bcache[hash_b].lock);
  b->refcnt--;
  b->lastuse = ticks;
  release(&bcache[hash_b].lock);
}

void
bpin(struct buf *b) {
  int hash_b = hash(b->blockno);
  acquire(&bcache[hash_b].lock);
  b->refcnt++;
  release(&bcache[hash_b].lock);
}

void
bunpin(struct buf *b) {
  int hash_b = hash(b->blockno);
  acquire(&bcache[hash_b].lock);
  b->refcnt--;
  release(&bcache[hash_b].lock);
}


/*
上一个是给每个cpu单独分配freelist，但这里不应该这样做，缓冲块应该是对所有cpu都可用的
*/