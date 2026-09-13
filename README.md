xv6 is a re-implementation of Dennis Ritchie's and Ken Thompson's Unix
Version 6 (v6).  xv6 loosely follows the structure and style of v6,
but is implemented for a modern RISC-V multiprocessor using ANSI C.

# Allocate Live List Node and SwapList Node
```C
struct liveListNode *
allocate_livelist_node(void)
{
  //printf("allocate live e asche!!\n") ;
  struct run *r = livelist_node_mem.freelist ;

  //need to acquire lock
  acquire(&livelist_node_mem.lock) ;
  if(livelist_node_mem.freelist == 0) { //if freelist is empty
    //need to allocate
    release(&livelist_node_mem.lock) ;
    char *mem = kalloc() ; // 4KB allocating
    char *m_end = mem + PGSIZE ; //
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
```
- The Allocate swapListNode is almost like allocating live list node
# Swapping Out
```
we need to know the reference count of the physical page.
if it's 0 then we need to free it from the physical memory
- 
- Swap out the page of the given node
- add the node to the swapped list
- remove the node from the live list
- free the physical page
- update the reference count
- update the swap count 

```
```C
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
    panic("pte not asdffound\n"); //if the pte is not found in the live list
  }
  live.liveCount--; //the PTE has been moved to DISK!!
  printf("<===============Removed from livePages,number of live pages : %d =====>\n ",live.liveCount) ;

  swapCount(s,4,ref_cnt) ;




  acquire(&refCount.lock);
  refCount.count[rppn] = 0; //setting the reference count to 0
  release(&refCount.lock); 
  kfree_helper((void*) PPN2PA(rppn)); //freeing the physical page
  //printf("=========Swap-Out done==========\n") ;
}
```
# Swap in Function
```
- The function ensures that the number of pages currently resident in memory. 
- live.liveCount does not exceed a maximum threshold MAX_LIVE_PAGE.
- If its already been swapped then remove from swapped list
- Add that into livelist
```
```C
void swap_in(int vpn, int process_id, uint64 *pte){
  //printf("comming to swap-in\n") ;
```
- swapping out nodes to swapin a new one

```C
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
    swap_out(t); // swapping out the last node because we need to swap in the new node
  }
```
- This loop searches the swapped list to find the node corresponding 
to the process and virtual page number that need to be swapped in.
```C
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

  while(n->next){ //traversing through the whole swapped list to find the node to be swapped in
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
  ```
- The loop searches for the node with a swap structure (sp) that matches the one just swapped in
```C
  n = swapped.list ; // again poiniting to head
  while(n->next){ // traversing through the whole swapped list to remove the node from the swapped list
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
      swapCount(s,2,0) ; //decrementing the reference count
      if(!swapCount(s,3,0)) // if the reference count is 0 then free the swap structure
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
  //how many processes are currently using the swapped-in page. 
  /*This is important for managing shared pages and ensuring 
    that pages are not prematurely freed if multiple processes reference them.
    */
  refCount.count[ppn] = ref_cnt; // 
  release(&refCount.lock);
}

```

# Adding Page Table Entry into Swapped List
```
- When a new process uses the page that was swapped out before by another process the swappedlist needs to refelct the change.The oldprocess_id is used to locate the swapped page in the list, and the newprocess_id is used to update the ownership or usage record.Typically happens when a child process inherits mem-mapping from parents.

```

```C
void addSwapped(pte_t *pte, int oldprocess_id, int newprocess_id, int vpn){
  //printf("comming to add-swapped\n") ;
  //does it need to be removed from Live-list?
  //No because addswapped is called in map-pages
  //when there's swapped bit is set(must've been) swapped out before!!
  //See swap_out function It sets the swappedbit
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
```
# Add Live Page
```C
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
```
```
As we are adding to livelist and swapped list that should be freed too
- RemoveFromSwapped
- RemoveFromLive
implemented on code
```

# Kfree-Updated
```C

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
  if(c <= 0){
    panic("kfree__");
  }
  acquire(&refCount.lock);
  refCount.count[ppn]--; 
  int count = refCount.count[ppn];
  release(&refCount.lock);
  if(count == 0){ // No process is using this page
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
  //checks if the address is page-aligned and 
  //within valid physical memory bounds.

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
```
# Free Page Count
```C
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
```

- There hadbeen some key changes in vm.c functions as we now have to deal with many bits 
.

# Traphandler in vm.c
- Called in userTrap() in trap.c

```C
    if(r_scause() == 0x0f || r_scause() == 0x0c || r_scause() == 0x0d){
      handled =  trapHandler(p->pagetable, p->pid);
      if(!handled){
        setkilled(p);
      }

    }
```

```C
int trapHandler(pagetable_t p, int process_id){
  //printf("trapHandler-called\n") ;
  uint64 va = r_stval();
  if(va >= MAXVA) return 0;
  pte_t *pte = walk(p,va,0,0);
  if(pte == 0){
    printf("pte 0");
    return 0;
  }
  acquireSlock();
  int vpn = VA2VPN(va);
  if(*pte & PTE_V){
    releaseSlock();
    return 1;
  }
  swap_in(vpn, process_id, pte);
  releaseSlock();
  return 1;
}
```
# Count of live Pages

```C
void Count(uint64 sz,int pid,pagetable_t pagetable) {
  pte_t *pte;
  uint64 i;
  int livecount = 0, swapcount = 0;
  for (i = 0; i < sz; i += PGSIZE)
  {
    if ((pte = walk(pagetable, i, 0,1)) == 0)
      panic("uvmcopy: pte should exist");
    if ((*pte & PTE_V) == 0 && (*pte & PTE_SWAPPED) == 0)
      panic("uvmcopy: page not present");

    if (*pte & PTE_SWAPPED)
    {
      swapcount++;
    }
    else if (*pte & PTE_V)
    {
      livecount++;
    }
  }

  printf("process_id: %d livePage :%d swappedPage: %d\n",pid,livecount,swapcount) ;
}
```

```
- There's an issue of some extra-pages
- It is due to initialization of 
    main()->
        kvminit()->
            kvmmake()->
                kvmmap() 
                ...
                ...

- The freerange() in kalloc.c should be called on kinit() to free 1<<15 (+-) pages 
- So when any program starts running it should show either 0 or some pages of kernel pagetable
```
# Free Range
```C
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
```




ACKNOWLEDGMENTS

xv6 is inspired by John Lions's Commentary on UNIX 6th Edition (Peer
to Peer Communications; ISBN: 1-57398-013-7; 1st edition (June 14,
2000)).  See also https://pdos.csail.mit.edu/6.1810/, which provides
pointers to on-line resources for v6.

The following people have made contributions: Russ Cox (context switching,
locking), Cliff Frey (MP), Xiao Yu (MP), Nickolai Zeldovich, and Austin
Clements.

We are also grateful for the bug reports and patches contributed by
Takahiro Aoyagi, Silas Boyd-Wickizer, Anton Burtsev, carlclone, Ian
Chen, Dan Cross, Cody Cutler, Mike CAT, Tej Chajed, Asami Doi,
eyalz800, Nelson Elhage, Saar Ettinger, Alice Ferrazzi, Nathaniel
Filardo, flespark, Peter Froehlich, Yakir Goaron, Shivam Handa, Matt
Harvey, Bryan Henry, jaichenhengjie, Jim Huang, Matúš Jókay, John
Jolly, Alexander Kapshuk, Anders Kaseorg, kehao95, Wolfgang Keller,
Jungwoo Kim, Jonathan Kimmitt, Eddie Kohler, Vadim Kolontsov, Austin
Liew, l0stman, Pavan Maddamsetti, Imbar Marinescu, Yandong Mao, Matan
Shabtay, Hitoshi Mitake, Carmi Merimovich, Mark Morrissey, mtasm, Joel
Nider, Hayato Ohhashi, OptimisticSide, Harry Porter, Greg Price, Jude
Rich, segfault, Ayan Shafqat, Eldar Sehayek, Yongming Shen, Fumiya
Shigemitsu, Cam Tenny, tyfkda, Warren Toomey, Stephen Tu, Rafael Ubal,
Amane Uehara, Pablo Ventura, Xi Wang, WaheedHafez, Keiichi Watanabe,
Nicolas Wolovick, wxdao, Grant Wu, Jindong Zhang, Icenowy Zheng,
ZhUyU1997, and Zou Chang Wei.


The code in the files that constitute xv6 is
Copyright 2006-2022 Frans Kaashoek, Robert Morris, and Russ Cox.

ERROR REPORTS

Please send errors and suggestions to Frans Kaashoek and Robert Morris
(kaashoek,rtm@mit.edu).  The main purpose of xv6 is as a teaching
operating system for MIT's 6.1810, so we are more interested in
simplifications and clarifications than new features.

BUILDING AND RUNNING XV6

You will need a RISC-V "newlib" tool chain from
https://github.com/riscv/riscv-gnu-toolchain, and qemu compiled for
riscv64-softmmu.  Once they are installed, and in your shell
search path, you can run "make qemu".
