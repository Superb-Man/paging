// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.
// #pragma once
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
  struct liveListNode* next;//link
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


//for root of the live linked list
struct {
  struct liveListNode* list;
  int liveCount ;
} live;

//for root of the swapped linked list
struct {
  struct swappedListNode* list ;
  int swappedCount ;
} swapped;

// function prototype
void kfree_helper(void *pa);
struct liveListNode *allocate_livelist_node(void);
struct swappedListNode *allocate_swappedlist_node(void);
void swap_out(struct liveListNode* n);
void addSwapped(pte_t *pte, int oldprocess_id, int newprocess_id, int vpn);
void swap_in(int vpn, int process_id, uint64 *pte);
void removeLive(int vpn, int process_id, uint64* pte);
void addLive(pte_t *pte, int process_id, int vpn, int h);
void removeFromSwapped(int process_id, int vpn, pte_t* pte);

//simple allocation prinpiple in linkedList (using freelist)
struct liveListNode *
allocate_livelist_node(void)
{
  //printf("allocate live e asche!!\n") ;
  struct run *r = livelist_node_mem.freelist ;

  //need to acquire lock
  acquire(&livelist_node_mem.lock) ;
  if(livelist_node_mem.freelist == 0) {
    //need to allocate
    release(&livelist_node_mem.lock) ;
    char *mem = kalloc() ; // 4KB allocating
    char *m_end = mem + PGSIZE ;
    for(; mem + sizeof(struct liveListNode) <= m_end ; mem+=sizeof(struct liveListNode)) {
      r = (struct run*) mem ;
      acquire(&livelist_node_mem.lock) ;
      r->next = livelist_node_mem.freelist ;
      livelist_node_mem.freelist = r ;
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
  struct run *r = swappedlist_node_mem.freelist ;

  //need to acquire lock
  acquire(&swappedlist_node_mem.lock) ;
  if(!(swappedlist_node_mem.freelist)) {
    //need to allocate
    release(&swappedlist_node_mem.lock) ;
    char *mem = kalloc() ; // 4KB allocating
    char *m_end = mem + PGSIZE ;
    for(; mem + sizeof(struct swappedListNode) <= m_end ; mem+=sizeof(struct swappedListNode)) {
      r = (struct run*) mem ;
      acquire(&swappedlist_node_mem.lock) ;
      r->next = swappedlist_node_mem.freelist ;
      swappedlist_node_mem.freelist = r ;
      release(&swappedlist_node_mem.lock) ;
    }
    acquire(&swappedlist_node_mem.lock) ;
    r = swappedlist_node_mem.freelist ;
  }
  swappedlist_node_mem.freelist = r->next ;
  release(&swappedlist_node_mem.lock) ;

  return (struct swappedListNode*) r ;
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
  swapout(s,(char*)pa);
  printf("<==============Swapped Out=================>\n") ;
  int ref_cnt = 0;
  int f = 0;
  int rppn = PTE2PPN(*n->pte) ;

  struct liveListNode* l = live.list ;//pointing to the head of live list
  if(l == 0)
    panic("live list head empty\n");
  // struct liveListNode* found;
  while(l->next){ // traverse through the whole livenode list
    //if physical page number matches then set the valid bit 0 and swapped bit to 1 ;
    if(rppn == PTE2PPN(*l->next->pte)){
      f = 1;
      *l->next->pte &= (~PTE_V);
      *l->next->pte |= PTE_SWAPPED;

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
      sn->process_id = l->next->process_id ;
      sn->pte = l->next->pte ;
      sn->sp = s ; //As we are swapping out we need to have that swap* s saved in our linked list
      sn->vpn = l->next->vpn ;
      if(swapped.list == 0)
        panic("swapped list head 0");

      //Now the next will be head->next(linked list)
      //Now This is a FIFO linked list
      //at each time we are inserting at the head of the list

      sn->next = swapped.list->next;
      swapped.list->next = sn;
      swapped.swappedCount++ ;
      //printf("Swapped out a pte and swapped size : %d\n\n",swapped.swappedCount) ;
      //================Freeing===================//
      struct liveListNode* t = l->next->next;
      //now the l->next node to be freed from the live list
      //we need to free
      struct run *rr ;
      if(l->next == 0) panic("swap-free") ;
      struct liveListNode *ll = l->next ;
      ll->next = 0 ;
      ll->process_id = 0 ;
      ll->pte = 0 ;
      ll->vpn = 0 ;
      rr = (struct run*) ll ;
      //Now acquire locks
      acquire(&livelist_node_mem.lock) ;
      rr->next = livelist_node_mem.freelist ;
      livelist_node_mem.freelist = rr ;
      release(&livelist_node_mem.lock) ;


      l->next = t ;
      //for referencing same Physical page number
      ++ref_cnt ;
    }
    else{
      l = l->next;
    }
  }
  if(!f){
    panic("pte not asdffound\n");
  }
  live.liveCount--; //the PTE has been moved to DISK!!
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
    panic("swap not found") ;
  }
}

void swap_in(int vpn, int process_id, uint64 *pte){
  //printf("comming to swap-in\n") ;
  //swapin requires the node to be added in live-pages
  while(live.liveCount >= MAX_LIVE_PAGE){
    struct liveListNode* t = live.list;
    if(!t) panic("live list head empty");
    else if(!t->next) panic("live list empty");
    t = t->next;
    //traversing to the end of the livelist
    while(t->next) {
      t = t->next;
    }
    swap_out(t);
  }
  if(*pte & PTE_V){
    panic("valid bit set\n");
  }
  struct swappedListNode* n;
  n = swapped.list ; //pointing to the head
  if(n == 0){
    panic("swap list empty\n");
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
        panic("matched!!");

      pte_t *pte = n->next->pte ;
      *pte = (PTE_FLAGS(*pte)) | (PA2PTE((uint64)mem)) | (PTE_V) ;
      *pte &= (~PTE_SWAPPED) ;

      int pid = n->next->process_id;
      int vp = n->next->vpn;

      struct swappedListNode *t = n->next->next;
      struct swappedListNode *tt = n->next ;
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
      //===========Freeing done===========//
      swapped.swappedCount-- ;
      n->next = t ;
      swapCount(s,2,0) ;
      if(!swapCount(s,3,0))
        swapfree(s);
      ++ref_cnt;
      //needs to be added on live list!!
      addLive(pte, pid, vp, 0);
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
    panic("valid bit is on");
  }
  struct swappedListNode *s , *t;
  // acquire(&swapped.lock);
  s = swapped.list;
  if(s == 0){
    panic("swapped list head empty\n");
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
    panic("rem from swap: swap not found\n");
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
    panic("liveListNode alloc");
  }
  nd->process_id = process_id;
  nd->pte = pte;
  nd->vpn = vpn;
  nd->next = 0;
  if(live.list == 0){
    panic("list head empty");
  }
  nd->next = live.list->next;
  live.list->next = nd;
  int ppn = PTE2PPN(*pte);
  if(ppn < 0 || ppn >= PAGE_COUNT){
    panic("invalid ppn");
  }
  live.liveCount++ ;
  printf("<=================Added livePages,number of live pages : %d ========>\n ",live.liveCount) ;
  //live_count++ ;
  // printf("Now live pages : %d\n", live_count) ;

  //printf("Ekhan porzonto aste parche??\n") ;
  if(live.liveCount >= MAX_LIVE_PAGE){
    //==============swapping out the last node=====================//
    //Swap out will only be called when there's unique live pages >= MAX-LIVE PAGES
    struct liveListNode* t = live.list;
    if(!t) panic("live list head empty");
    else if(!t->next) panic("live list empty");
    t = t->next;
    //traversing to the end of the livelist
    while(t->next) {
      t = t->next;
    }
    printf("<================More than live_list size, Need to be swapped out ==================>\n") ;
    swap_out(t);
  }
  //printf("reaching end of addlive\n") ;

}

void removeLive(int vpn, int process_id, uint64* pte){
  //printf("comming to removelive\n") ;
  if(*pte & PTE_SWAPPED)
    panic("swapped bit on _") ;

  struct liveListNode* n;
  *pte &= PTE_V ;

  if(live.list == 0)
    panic("live list head empty\n");

  n = live.list;
  int f = 0;
  while(n->next){
    if(n->next->pte == pte){
      struct liveListNode* t = n->next;
      n->next = n->next->next;
      //============Freeing the node from Livelist================//
      struct run *rr ;
      if(t == 0) panic("swap-free") ;
      t->next = 0 ;
      t->process_id = 0 ;
      t->pte = 0 ;
      t->vpn = 0 ;
      rr = (struct run*) t ;
      //Now acquire locks
      acquire(&livelist_node_mem.lock) ;
      rr->next = livelist_node_mem.freelist ;
      livelist_node_mem.freelist = rr ;
      release(&livelist_node_mem.lock) ;
      //===============Freeing from livelist done=============//

      f = 1;

    }
    else
      n = n->next;
  }

  if(!f)
    panic("pte not found_");
  live.liveCount--;
  //printf("Removed from livePages,number of live pages : %d\n ",live.liveCount) ;

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


  live.list = allocate_livelist_node() ;
  //printf("allocated initial node at live list\n") ;

  if(live.list == 0) // is not alloacted
    panic("live head empty");
  live.liveCount = 0 ;
  //per node variables
  live.list->pte = 0 ;
  live.list->process_id = 0 ;
  live.list->next = 0 ;
  live.list->vpn = 0 ;

  swapped.list = allocate_swappedlist_node();
  if(swapped.list == 0) //is not allocated
    panic("swapped list init");
  swapped.list->next = 0 ;
  swapped.list->sp = 0 ;
  swapped.list->vpn = 0 ;
  swapped.list->process_id = 0 ;
  swapped.list->pte = 0;
  swapped.swappedCount = 0 ;
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
void kfree(void *pa){
  //printf("called\n") ;
  uint64 ppn = PA2PPN((uint64)pa) ;
  acquire(&refCount.lock);
  int c = refCount.count[ppn];
  release(&refCount.lock);
  if(c < 0){
    return;
    // panic("kfree__");
  }
  acquire(&refCount.lock);
  refCount.count[ppn]--;
  int count = refCount.count[ppn];
  release(&refCount.lock);
  if(count == 0){
    //uint64 pa2 = PPN2PA(ppn);
    kfree_helper((void*)((uint64)PPN2PA(ppn)));
  }
  kfree_helper(pa);
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
    memset((char*)(r + 1), 5, PGSIZE - sizeof(*r));
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