// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.
#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"
#include "sleeplock.h"
// #include "swap.c"
// Given on assignment
#define MAX_LIVE_PAGE 50
#define PAGE_COUNT 1<<15
int live_count = 0 ;
void kfree_helper(void *pa);
// void swapListSize();

struct sleeplock slock;
struct run {
  struct run *next;
};
struct {
  char count[PAGE_COUNT];
  struct spinlock lock;
} refCount;

//implementation of linked-list

struct liveListNode {
  pte_t *pte; //the page table entry
  int process_id;//for which process
  int vpn;//virtual page number
  struct liveListNode* next;
  struct liveListNode* prev;
};
// must have a swap* s structure to know either it is swapped before!!

struct swappedListNode{
  int process_id;//for which process
  int vpn;// virtual page number
  struct swap* sp;//the important swap structure
  struct swappedListNode* next;//link for next node
  pte_t *pte; // the page table entry pointer
};

//implementing freelist
struct {
  struct spinlock lock;
  struct run *freelist;
} livelist_node_mem, swappedlist_node_mem;

//simple allocation prinpiple in linkedList (using freelist)
struct liveListNode *
allocate_livelist_node(void)
{
  //printf("allocate live e asche!!\n") ;
  struct run *r;
  //need to acquire lock
  acquire(&livelist_node_mem.lock) ;
  r = livelist_node_mem.freelist;
  if(r == 0) {
    //need to allocate
    release(&livelist_node_mem.lock) ;
    char *mem = kalloc() ; // 4KB allocating
    char *m_end = mem + PGSIZE ;
    for(; mem + sizeof(struct liveListNode) <= m_end ; mem+=sizeof(struct liveListNode)) {
      struct run *chunk = (struct run*) mem ;
      acquire(&livelist_node_mem.lock) ;
      chunk->next = livelist_node_mem.freelist;
      livelist_node_mem.freelist = chunk ;
      release(&livelist_node_mem.lock) ;
    }
    acquire(&livelist_node_mem.lock) ;
    r = livelist_node_mem.freelist ;
  }
  livelist_node_mem.freelist = r->next ;
  release(&livelist_node_mem.lock) ;
  //printf("why can't allocate?\n") ;
  return (struct liveListNode*) r ;
}

struct swappedListNode *
allocate_swappedlist_node(void)
{
  struct run *r;

  //need to acquire lock
  acquire(&swappedlist_node_mem.lock) ;
  r = swappedlist_node_mem.freelist;
  if(r == 0) {

    //need to allocate
    release(&swappedlist_node_mem.lock) ;
    char *mem = kalloc() ; // 4KB allocating
    char *m_end = mem + PGSIZE ;
    for(; mem + sizeof(struct swappedListNode) <= m_end ; mem+=sizeof(struct swappedListNode)) {
      struct run *chunk = (struct run*) mem ;
      acquire(&swappedlist_node_mem.lock) ;
      chunk->next = swappedlist_node_mem.freelist ;
      swappedlist_node_mem.freelist = chunk ;
      release(&swappedlist_node_mem.lock) ;
    }
    acquire(&swappedlist_node_mem.lock) ;
    r = swappedlist_node_mem.freelist ;
  }
  swappedlist_node_mem.freelist = r->next ;
  release(&swappedlist_node_mem.lock) ;

  return (struct swappedListNode*) r ;
}
//for root of the live linked list
struct {
  struct liveListNode* head;  // MRU
  struct liveListNode* tail;  // LRU
  int liveCount;
} live;

//for root of the swapped linked list
struct {
  struct swappedListNode* list ;
  int swappedCount ;
} swapped;

// ============LRU helpers function====================

static inline void 
recycle_live_node(struct liveListNode* n){
  if(!n) return;
  n->next = 0;
  n->prev = 0;
  n->pte  = 0;
  n->process_id = 0;
  n->vpn = 0;
  struct run *rr = (struct run*)n;
  acquire(&livelist_node_mem.lock);
  rr->next = livelist_node_mem.freelist;
  livelist_node_mem.freelist = rr;
  release(&livelist_node_mem.lock);
}

void
unlink_live_node(struct liveListNode* n){
  if(!n) return;
  if(n->prev) 
    n->prev->next = n->next;
  else 
    live.head = n->next; 
  
  if(n->next) 
    n->next->prev = n->prev;
  else 
    live.tail = n->prev;  
  live.liveCount--;
}

void 
push_head_live(struct liveListNode* n){
  n->prev = 0;
  n->next = live.head;

  if(live.head) 
    live.head->prev = n;
  live.head = n;

  if(!live.tail) 
    live.tail = n;
  
  live.liveCount++;
}

void 
moveToHead(struct liveListNode* n){
  if(!n || live.head == n) 
    return;
  if(n->prev) 
    n->prev->next = n->next;
  else        
    live.head = n->next;
  if(n->next) 
    n->next->prev = n->prev;
  else        
    live.tail = n->prev;

  // push to head
  n->prev = 0;
  n->next = live.head;
  
  if(live.head) 
    live.head->prev = n;
  live.head = n;
  if(!live.tail) 
    live.tail = n;
}

void touch_pte(pte_t *pte){
  struct liveListNode* cur = live.head;
  while(cur){
    if(cur->pte == pte){ // found matching pte
      moveToHead(cur);
      return;
    }
    cur = cur->next;
  }
}

void touch_ppn(int ppn){
  struct liveListNode* cur = live.head;
  while(cur){
    if(PTE2PPN(*cur->pte) == ppn){
      moveToHead(cur);
      return;
    }
    cur = cur->next;
  }
}

void swap_out(struct liveListNode* n){
  //printf("comming to swap-out\n") ;
  if(n == 0){
    panic("null pointer to in swap_out");
  }
  struct swap *s = swapalloc();
  if(s == 0)
    panic("swap not allocated") ;
  uint64 set = *n->pte & PTE_SWAPPED ;
  if(set){
    panic("swapped bit on");
  }
  //printf("========swapping out=========\n") ;
  uint64 pa = PTE2PA(*n->pte) ;
  int rppn = PTE2PPN(*n->pte) ;
  swapout(s,(char*)pa);
  printf("<==============Swapped Out (ppn=%d)=================>\n", rppn);
  int ref_cnt = 0;
  int f = 0;

  struct liveListNode* l = live.head ;//pointing to the head of live list
  if(l == 0)
    panic("swap_out: live list head empty\n");
  // struct liveListNode* found;
  while(l){ // traverse through the whole livenode list
    //if physical page number matches then set the valid bit 0 and swapped bit to 1 ;
    struct liveListNode* next = l->next; // save next because we may unlink l
    if(rppn == PTE2PPN(*l->pte)){
      f = 1;
      *l->pte &= (~PTE_V);
      *l->pte |= PTE_SWAPPED;

      // struct liveListNode* curr = l->next;
      //now copying the l->next node to cur and
      //adding it to swappedlist
      //before we need to allocate a node using freelist
      //creating a temporary node

      struct swappedListNode* sn = 0 ;
      // panic("here-panic") ;
      sn = allocate_swappedlist_node();
      if(sn == 0)
        panic("swapped list node is not alloacted");
      sn->process_id = l->process_id ;
      sn->pte        = l->pte ;
      sn->vpn        = l->vpn ;
      sn->sp         = s ; //As we are swapping out we need to have that swap* s saved in our linked list

      if(swapped.list == 0)
        panic("swap_out: swapped list head null");

      //Now the next will be head->next(linked list)
      //Now This is a FIFO linked list
      //at each time we are inserting at the head of the list

      sn->next = swapped.list->next;
      swapped.list->next = sn;
      swapped.swappedCount++ ;
      //printf("Swapped out a pte and swapped size : %d\n\n",swapped.swappedCount) ;
      //================Freeing===================//
      unlink_live_node(l);
      recycle_live_node(l);

      ref_cnt++;
    }
    l = next;
  }
  if(!f)
    panic("swap_out: no matching PTEs for ppn");
    
  printf("<===============Removed from livePages,number of live pages : %d =====>\n ",live.liveCount) ;

  swapCount(s,4,ref_cnt) ;




  acquire(&refCount.lock);
  refCount.count[rppn] = 0;
  release(&refCount.lock);
  kfree_helper((void*) PPN2PA(rppn));
  //printf("=========Swap-Out done==========\n") ;
}

void addSwapped(pte_t *pte, int oldprocess_id, int newprocess_id, int vpn){
  //printf("comming to add-swapped\n") ;
  //does it need to be removed from Live-list?
  //No because addswapped is called in map-pages
  //when there's swapped bit is set(must've been) swapped out before!!
  //See swap_out function
  //It sets the swappedbit
  if(*pte & PTE_V){
    panic("valid bit on");
  }
  struct swappedListNode* new = allocate_swappedlist_node() ;
  if(!new) panic("swapped node alloc!") ;
  new->next = 0 ;
  new->sp = 0 ;
  new->process_id = newprocess_id ;
  new->pte = pte ;
  new->vpn = vpn ;

  if(swapped.list == 0)
    panic("swapped list head null");

  struct swappedListNode* curr = swapped.list ;//pointing to the head
  int f = 0;
  while(curr->next){
    if(curr->next->process_id == oldprocess_id ){
      int x = curr->next->vpn ;
      if(x == vpn){
        //using the same swap structure
        new->sp = curr->next->sp;
        //for fork()
        //without fork ei condition jiboneo execute hobe na!!
        //============KRV SIR DEKHBEN=============//
        //the function is called on map-pages
        //map-pages is called on uvm-copy
        //uvm-copy is called on fork()
        //So we need to update the reference cnt because we don't want to free it if some child process also use it
        //That's why iterating through and checking parent_process_id(old) with the live_list_process_ids
        swapCount(new->sp , 1, 0) ;
        f = 1;
        break ;
      }
    }
    curr = curr->next;
  }
  if(f) {
    new->next = swapped.list->next;
    swapped.list->next = new ;
    swapped.swappedCount++ ;
  }
  else {
    panic("addSwapped: swap not found");
  }
}

void swap_in(int vpn, int process_id, uint64 *pte){
  //printf("comming to swap-in\n") ;
  //swapin requires the node to be added in live-pages
  while(live.liveCount >= MAX_LIVE_PAGE){
    if (!live.tail)
      panic("swap_in : live tail is null in swap_in");
    swap_out(live.tail);
  }
  if(*pte & PTE_V){
    panic("valid bit set\n");
  }
  struct swappedListNode* n;
  n = swapped.list ; //pointing to the head
  if(n == 0){
    panic("swap_in : swap list empty\n");
  }
  int f = 0;
  struct swap *s;
  char* mem ;
  int ref_cnt = 0 ;
  //Now what??
  while(n->next){
    //the condition is needed for fork()
    if(n->next->process_id == process_id && n->next->vpn == vpn){
      f = 1 ;
      //for swappin we need to allocate memory
      mem =(char *) kalloc();
      if(n->next->sp == 0){
        panic("sp 0\n");
      }
      swapin(mem,n->next->sp);
      printf("<==============Swapped In=================>\n") ;
      //printf("Swapped in\n") ;
      s = n->next->sp;
      break;
    }
    else
      n = n->next;
  }
  if(f == 0){
    panic("swap in: swap not found\n");
  }

  n = swapped.list ; // again poiniting to head
  while(n->next){
    if(n->next->sp == s){
      if(s == 0)
        panic("swap_in : swap is null");

      pte_t *pte = n->next->pte ;
      *pte = (PTE_FLAGS(*pte)) | (PA2PTE((uint64)mem)) | (PTE_V) ;
      *pte &= (~PTE_SWAPPED) ;

      int pid = n->next->process_id;
      int vp = n->next->vpn;

      struct swappedListNode *tt = n->next ; // victim node
      if(tt == 0)
        panic("swap_in : swapList Node is null");
      n->next = tt->next;

      swapCount(s, 2, 0); // decrement swap count reference
      if(!swapCount(s,3,0)) // if no more references
        swapfree(s);
      ++ref_cnt;

      // recycle the swaped node
      tt->next       = 0 ;
      tt->process_id = 0 ;
      tt->vpn        = 0 ;
      tt->sp         = 0 ;
      struct run *rr = (struct run*) tt ;
      acquire(&swappedlist_node_mem.lock) ;
      rr->next = swappedlist_node_mem.freelist ;
      swappedlist_node_mem.freelist = rr ;
      release(&swappedlist_node_mem.lock) ;
      //===========Freeing done===========//
      swapped.swappedCount-- ;
      
      struct liveListNode* nd = allocate_livelist_node();
      if(!nd) 
        panic("swap_in: live node alloc");
      nd->pte = pte;
      nd->process_id = pid;
      nd->vpn = vp;
      nd->prev = 0; nd->next = 0;
      push_head_live(nd);
    }
    else
      n = n->next;
  }
  acquire(&refCount.lock);
  int ppn = PA2PPN(mem);
  refCount.count[ppn] = ref_cnt;
  release(&refCount.lock);
}

void removeFromSwapped(int process_id, int vpn, pte_t* pte){
  //printf("comming to swapremovedfrom\n") ;
  //does it need to be added in Live-pages?
  //No because it's only called when uvmunmap is called
  //basically it frees the physical address
  //so need to swapfree on that particular swap*
  if(*pte & PTE_V){
    panic("removeFromSwapped : valid bit is on");
  }
  struct swappedListNode *s , *t;
  // acquire(&swapped.lock);
  s = swapped.list;
  if(s == 0){
    panic("removeFromSwapped : swapped list head empty\n");
  }
  int f = 0;
  while(s->next){
    //the same type of iteration like addswapped, this time we just need to remove it!!
    if(s->next->vpn == vpn && s->next->process_id == process_id &&  s->next->pte == pte){
      f = 1;
      t = s->next;
      s->next = t->next;
      swapCount(t->sp,2,0) ;
      //we are freeing the swap structure finally!!

      if(!swapCount(t->sp,3,0)){
        swapfree(t->sp) ;
      }
      struct swappedListNode *tt = t ;
      struct run *rr ;
      //======Freeing===========//
      //freeing from swappedlist
      if(!tt)
        panic("swapfree");
      tt->next = 0 ;
      tt->process_id = 0 ;
      tt->vpn = 0 ;
      tt->sp = 0 ;
      rr = (struct run*) tt ;
      acquire(&swappedlist_node_mem.lock) ;
      rr->next = swappedlist_node_mem.freelist ;
      swappedlist_node_mem.freelist = rr ;
      release(&swappedlist_node_mem.lock) ;
      swapped.swappedCount-- ;
      //==========Freeing done===========//
      break;
    }
    else
      s = s->next;
  }
  if(!f){
    panic("removeFromSwapped : swap not found\n");
  }
}

void addLive(pte_t *pte, int process_id, int vpn, int h){
  //printf("comming to addlive\n") ;

  if(*pte & PTE_SWAPPED){
    panic("swapped bit on");
  }

  //printf("hoise\n") ;
  struct liveListNode* nd = allocate_livelist_node();
  //printf("allocate hoise addlive e \n") ;
  if(nd == 0){
    panic("addLive : liveListNode alloc");
  }
  nd->process_id  = process_id;
  nd->pte         = pte;
  nd->vpn         = vpn;
  nd->next        = 0;

  int ppn = PTE2PPN(*pte);
  if(ppn < 0 || ppn >= PAGE_COUNT){
    panic("invalid ppn");
  }
  push_head_live(nd);

  printf("<=================Added livePages (LRU), number of live pages : %d ========>\n", live.liveCount);

  // Evict until within capacity
  while(live.liveCount > MAX_LIVE_PAGE){
    if(!live.tail) 
      panic("addLive : live list empty while over capacity");
    printf("<================Over capacity; swapping out LRU (tail) ==================>\n");
    swap_out(live.tail);
  }
  //printf("reaching end of addlive\n") ;

}

void removeLive(int vpn, int process_id, uint64* pte){
  //printf("comming to removelive\n") ;
  if(*pte & PTE_SWAPPED)
    panic("swapped bit on _") ;

  struct liveListNode* cur;
  *pte &= PTE_V ;

  if(live.head == 0)
    panic("live list head empty\n");

  cur = live.head;
  int f = 0;
  while(cur){
    if(cur->pte == pte) {
      unlink_live_node(cur);
      recycle_live_node(cur);
      f = 1;
      break;
    }
    cur = cur->next;
  }

  if(!f)
    panic("pte not found_");

}




void
pageCountInfo(){
  int c = 0;
  struct swappedListNode* n;
  printf("===========stats===========\n");
  n = swapped.list->next;
  while(n != 0){
    n = n->next;
    ++c;
  }
  printf("live list size     : %d\n",live.liveCount) ;
  printf("swapped list size  : %d\n ",c) ;
}


void inc(uint64 ppn){
  if(ppn < 0 || ppn >= PAGE_COUNT)
    panic("ref count");
  acquire(&refCount.lock);
  refCount.count[ppn]++;
  release(&refCount.lock);
}


void initRefCount(){
  initlock(&refCount.lock, "refCount");
  for(int i = 0; i < PAGE_COUNT; ++i)
      refCount.count[i] = -1;
}

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.


struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  initRefCount();
  //printf("kinit\n") ;
  freerange(end, (void*)PHYSTOP);
  //printf("freerange\n") ;
  initlock(&livelist_node_mem.lock, "livelist_node_mem");
  livelist_node_mem.freelist = 0 ;// Initially pointing to null
  //printf("livelist-init\n") ;
  initlock(&swappedlist_node_mem.lock, "swappedlist_node_mem");
  swappedlist_node_mem.freelist = 0 ;


  live.head = 0 ;
  live.tail = 0 ;
  live.liveCount = 0 ;
  //per node variables

  swapped.list = allocate_swappedlist_node();
  if(swapped.list == 0) //is not allocated
    panic("kinit : swapped list init");
  swapped.list->next       = 0 ;
  swapped.list->sp         = 0 ;
  swapped.list->vpn        = 0 ;
  swapped.list->process_id = 0 ;
  swapped.list->pte        = 0;
  swapped.swappedCount     = 0 ;
  //printf("swapped-list-init\n") ;
  //printf("init-sleeplock-start\n") ;
  initsleeplock(&slock, "sleepLock");
  //printf("init-sleeplock-end\n") ;
}

void
freerange(void *pa_start, void *pa_end)
{

  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE){
    refCount.count[PA2PPN((uint64)p)] = 0;
    kfree_helper((void*)(p)) ;
  }
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
int cnt = 0 ;
void 
kfree(void *pa){
  //printf("called\n") ;
  uint64 ppn = PA2PPN((uint64)pa) ;
  acquire(&refCount.lock);
  int c = refCount.count[ppn];
  release(&refCount.lock);
  if(c <= 0){
    panic("kfree__");
  }
  acquire(&refCount.lock);
  refCount.count[ppn]--;
  int count = refCount.count[ppn];
  release(&refCount.lock);
  if(count == 0){
    //uint64 pa2 = PPN2PA(ppn);
    kfree_helper((void*)((uint64)PPN2PA(ppn)));
  }
}


/**
 * We need to count the references of PPN for any kalloc and kfree
*/


void
kfree_helper(void *pa)
{
  cnt++ ;
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
  //printf("%d k free called\n", cnt) ;
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  if(r){
    memset((char*)r, 5, PGSIZE); // fill with junk
    inc(PA2PPN((uint64)r));
  }
  return (void*)r;
}

int freePageCount(){
  int c = 0;
  struct run *cur ;
  acquire(&kmem.lock);
  cur = kmem.freelist;
  while(cur){
    ++c;
    cur = cur->next;
  }
  release(&kmem.lock);

  printf("free page count in freelist is %d\n",c) ;

  //return c ;

  c = 0 ;
  for(int i = 0; i < PAGE_COUNT; ++i){
    acquire(&refCount.lock);
    if(refCount.count[i] == 0) ++c;
    release(&refCount.lock);
  }
  printf("free page count in ref-count is %d\n",c) ;

  return 1 ;
}
void acquireSlock(){
  acquiresleep(&slock);
}
void releaseSlock(){
  releasesleep(&slock);
}