#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"

/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

// extern char refCount[];

extern char etext[];  // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S

// Make a direct-map page table for the kernel.
pagetable_t
kvmmake(void)
{
  pagetable_t kpgtbl;

  kpgtbl = (pagetable_t) kalloc();
  memset(kpgtbl, 0, PGSIZE);

  // uart registers
  kvmmap(kpgtbl, UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap(kpgtbl, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

  // PLIC
  kvmmap(kpgtbl, PLIC, PLIC, 0x400000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(kpgtbl, KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap(kpgtbl, (uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(kpgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

  // allocate and map a kernel stack for each process.
  proc_mapstacks(kpgtbl);
  
  return kpgtbl;
}

// Initialize the one kernel_pagetable
void
kvminit(void)
{
  kernel_pagetable = kvmmake();
}

// Switch h/w page table register to the kernel's page table,
// and enable paging.
void
kvminithart()
{
  // wait for any previous writes to the page table memory to finish.
  sfence_vma();

  w_satp(MAKE_SATP(kernel_pagetable));

  // flush stale entries from the TLB.
  sfence_vma();
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.
pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc,int enq)
{
  if(va >= MAXVA)
    panic("walk");

  for(int level = 2; level > 0; level--) {
    pte_t *pte = &pagetable[PX(level, va)];
    if(*pte & PTE_V) {
      pagetable = (pagetable_t)PTE2PA(*pte);
    } else {
      if(!alloc || (pagetable = (pde_t*)kalloc()) == 0)
        return 0;
      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V;
      // if(enq)
      //   enqueue(pte);

    }
  }
  return &pagetable[PX(0, va)];
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64
walkaddr(pagetable_t pagetable, uint64 va, int process_id)
{
  pte_t *pte;
  uint64 pa;

  if(va >= MAXVA)
    return 0;

  pte = walk(pagetable, va, 0, 0);
  if(pte == 0)
    return 0;

  acquireSlock();
  if((*pte & PTE_V) == 0){
    if(*pte & PTE_SWAPPED){
      swap_in(VA2VPN(va), process_id, pte);
    }
    else{
      releaseSlock();
      return 0;
    }
  }
  releaseSlock();
  if((*pte & PTE_U) == 0)
    return 0;
  pa = PTE2PA(*pte);
  return pa;
}

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void
kvmmap(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm)
{
  if(mappages(kpgtbl, va, sz, pa, perm, 0, -1) != 0)
    panic("kvmmap");
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned. Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
int
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm, int enq, int process_id)
{
  uint64 a, last;
  pte_t *pte;

  if(size == 0)
    panic("mappages: size");
  
  a = PGROUNDDOWN(va);
  last = PGROUNDDOWN(va + size - 1);
  for(;;){
    if((pte = walk(pagetable, a, 1, enq)) == 0)
      return -1;


    if(*pte & PTE_V)
      panic("mappages: remap");
    *pte = PA2PTE(pa) | perm | PTE_V;


    if(enq){
        if(process_id > 1)
          acquireSlock();
      if(*pte & PTE_SWAPPED){
        *pte &= (~PTE_V);
        addSwapped(pte, enq, process_id, VA2VPN(a));
      }
      else{
        if(!(*pte & PTE_X))
          addLive(pte, process_id, VA2VPN(a), 0);
      }

      if(process_id > 1)
            releaseSlock();

    }
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. The mappings must exist.
// Optionally free the physical memory.
void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free, int process_id)
{
  // printf("uvmunmap Entry\n");

  uint64 a;
  pte_t *pte;

  if((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  for(a = va; a < va + npages*PGSIZE; a += PGSIZE){
    if((pte = walk(pagetable, a, 0,0)) == 0){
      panic("uvmunmap: walk");
    }

    if(((*pte & PTE_V) || (*pte & PTE_SWAPPED)) == 0){
      panic("uvmunmap: not mapped");
    }
    if(PTE_FLAGS(*pte) == PTE_V)
      panic("uvmunmap: not a leaf");
    if(do_free){
      acquireSlock();
      uint64 pa = PTE2PA(*pte);
      if((*pte) & PTE_SWAPPED){
        removeFromSwapped(process_id, VA2VPN(a), pte);
      }
      else{
        if(!(*pte & PTE_X))
          removeLive(VA2VPN(a), process_id, pte);
        kfree((void*)pa);
      }
      releaseSlock();
    }
    *pte = 0;
  }
  // printf("uvmunmap exit\n");

}

// create an empty user page table.
// returns 0 if out of memory.
pagetable_t
uvmcreate()
{
  pagetable_t pagetable;
  pagetable = (pagetable_t) kalloc();
  if(pagetable == 0)
    return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// Load the user initcode into address 0 of pagetable,
// for the very first process.
// sz must be less than a page.
void
uvmfirst(pagetable_t pagetable, uchar *src, uint sz, int process_id)
{
  char *mem;

  if(sz >= PGSIZE)
    panic("uvmfirst: more than a page");
  mem = kalloc();

  memset(mem, 0, PGSIZE);
  mappages(pagetable, 0, PGSIZE, (uint64)mem, PTE_W|PTE_R|PTE_X|PTE_U,1, process_id);
  memmove(mem, src, sz);
}

// Allocate PTEs and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz, int xperm, int process_id)
{
  char *mem;
  uint64 a;

  if(newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for(a = oldsz; a < newsz; a += PGSIZE){
    mem = kalloc();
    if(mem == 0){
      uvmdealloc(pagetable, a, oldsz, process_id);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if(mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_R|PTE_U|xperm,1 ,process_id) != 0){
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz, process_id);
      return 0;
    }
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz, int process_id)
{
  if(newsz >= oldsz)
    return oldsz;

  if(PGROUNDUP(newsz) < PGROUNDUP(oldsz)){
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1, process_id);
  }

  return newsz;
}

void pageCount(pagetable_t pagetable, int level, int* livePageCount, int* swappedPageCount) {
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    if(pte & PTE_V){
      if(level == 2){
        *livePageCount += 1;
      }
      else{
        uint64 child = PTE2PA(pte);
        pageCount((pagetable_t)child, level + 1, livePageCount, swappedPageCount);
      }
    }
    else if(pte & PTE_SWAPPED){
      //swapped on thakle valid off thake
      *swappedPageCount += 1;
    }
  }
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
void
freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    if((pte & PTE_V) && (pte & (PTE_R|PTE_W|PTE_X)) == 0){
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    } else if(pte & PTE_V){
      panic("freewalk: leaf");
    }
  }
  kfree((void*)pagetable);
}

// Free user memory pages,
// then free page-table pages.
void
uvmfree(pagetable_t pagetable, uint64 sz, int process_id)
{
  // printf("uvmfree Entry\n");
  if(sz > 0){
    uvmunmap(pagetable, 0, PGROUNDUP(sz)/PGSIZE, 1, process_id);
  }
  freewalk(pagetable);

}

// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz,int oldprocess_id, int newprocess_id, struct spinlock* lk)
{
  pte_t *pte, *newpte;
  uint64 pa, i;
  uint64 flags;
  // printf("uvm copy entry");
  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walk(old, i, 0,0)) == 0)
      panic("uvmcopy: pte should exist");

    // The child gets a private physical page. Keep the source resident
    // through the copy and register the child's mapping before unlock.
    acquireSlock();
    if((*pte & PTE_V) == 0){
      if(*pte & PTE_SWAPPED){
        swap_in(VA2VPN(i),oldprocess_id, pte);
      }
      else
        panic("uvmcopy: page not present");
    }

    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    char *mem ;
    mem = kalloc() ;
    if(mem == 0){
      releaseSlock();
      goto err;
    }
    memmove(mem, (char*)pa, PGSIZE);
    if(mappages(new, i, PGSIZE, (uint64)mem, flags, 0, newprocess_id) != 0){
      kfree(mem);
      releaseSlock();
      goto err;
    }
    newpte = walk(new, i, 0, 0);
    if((flags & PTE_X) == 0)
      addLive(newpte, newprocess_id, VA2VPN(i), 0);
    releaseSlock();
  }
  return 0;

 err:
  uvmunmap(new, 0, i / PGSIZE, 1, newprocess_id);
  return -1;
}

int
uvmmirror(pagetable_t old, pagetable_t new, uint64 oldsz, uint64 newsz, int process_id)
{
  pte_t *pte, *newpte;
  uint64 pa;
  uint64 i;
  uint flags;

  oldsz = PGROUNDUP(oldsz);
  for(i = oldsz; i < newsz; i += PGSIZE){
    if((pte = walk(old, i, 0, 0)) == 0)
      panic("uvmmirror: pte should exist");

    // Keep the source resident until its new mapping is registered.
    acquireSlock();
    if((*pte & PTE_V) == 0){
      if(*pte & PTE_SWAPPED)
        swap_in(VA2VPN(i), process_id, pte);
      else
        panic("uvmmirror: page not present");
    }
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    inc(PTE2PPN(*pte));
    if(mappages(new, i, PGSIZE, pa, flags, 0, process_id) != 0){
      kfree((void *)pa);
      releaseSlock();
      goto err;
    }
    newpte = walk(new, i, 0, 0);
    if((flags & PTE_X) == 0)
      addLive(newpte, process_id, VA2VPN(i), 0);
    releaseSlock();
  }
  return (int)newsz;

err:
  if(i > oldsz)
    uvmunmap(new, oldsz, (i - oldsz) / PGSIZE, 1, process_id);
  return -1;
}

uint64
uvmallocmirror(pagetable_t parentptable, pagetable_t threadptable,
               uint64 oldsz, uint64 newsz, int process_id)
{
  pte_t *pte, *newpte;
  uint64 pa;
  uint64 i;
  uint flags;

  if(newsz < oldsz)
    return oldsz;

  for(i = PGROUNDUP(oldsz); i < newsz; i += PGSIZE){
    if((pte = walk(parentptable, i, 0, 0)) == 0)
      panic("uvmallocmirror: pte should exist");

    acquireSlock();
    if((*pte & PTE_V) == 0){
      if(*pte & PTE_SWAPPED)
        swap_in(VA2VPN(i), process_id, pte);
      else
        panic("uvmallocmirror: page not present");
    }
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    inc(PTE2PPN(*pte));
    if(mappages(threadptable, i, PGSIZE, pa, flags, 0, process_id) != 0){
      kfree((void *)pa);
      releaseSlock();
      uvmdeallocmirror(threadptable, i, oldsz, process_id);
      return 0;
    }
    newpte = walk(threadptable, i, 0, 0);
    if((flags & PTE_X) == 0)
      addLive(newpte, process_id, VA2VPN(i), 0);
    releaseSlock();
  }
  return newsz;
}

uint64
uvmdeallocmirror(pagetable_t pagetable, uint64 oldsz, uint64 newsz, int process_id)
{
  if(newsz >= oldsz)
    return oldsz;

  if(PGROUNDUP(newsz) < PGROUNDUP(oldsz)){
    uint64 npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1, process_id);
  }

  return newsz;
}



// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void
uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  
  pte = walk(pagetable, va, 0, 0);
  if(pte == 0)
    panic("uvmclear");
  *pte &= ~PTE_U;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int
copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len, int process_id)
{
  if(dstva >= MAXVA)
    return -1;
  uint64 n, va0, pa0;
  while(len > 0){
    va0 = PGROUNDDOWN(dstva);
    pa0 = walkaddr(pagetable, va0, process_id);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (dstva - va0);
    if(n > len)
      n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int
copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len, int process_id)
{
  uint64 n, va0, pa0;

  while(len > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0, process_id);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if(n > len)
      n = len;
    memmove(dst, (void *)(pa0 + (srcva - va0)), n);

    len -= n;
    dst += n;
    srcva = va0 + PGSIZE;
  }
  return 0;
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int
copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max, int process_id)
{
  uint64 n, va0, pa0;
  int got_null = 0;

  while(got_null == 0 && max > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0, process_id);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if(n > max)
      n = max;

    char *p = (char *) (pa0 + (srcva - va0));
    while(n > 0){
      if(*p == '\0'){
        *dst = '\0';
        got_null = 1;
        break;
      } else {
        *dst = *p;
      }
      --n;
      --max;
      p++;
      dst++;
    }

    srcva = va0 + PGSIZE;
  }
  if(got_null){
    return 0;
  } else {
    return -1;
  }
}

int trapHandler(pagetable_t p, int process_id){
  //printf("trapHandler-called\n") ;
  uint64 va = r_stval();
  if(va >= MAXVA) return 0;
  pte_t *pte = walk(p,va,0,0);
  if(pte == 0 || (*pte & PTE_SWAPPED) == 0)
    return 0;
  acquireSlock();
  int vpn = VA2VPN(va);
  if(*pte & PTE_SWAPPED)
    swap_in(vpn, process_id, pte);
  else if((*pte & PTE_V) == 0){
    releaseSlock();
    return 0;
  }
  releaseSlock();
  return 1;
}

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
