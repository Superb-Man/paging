#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "proc.h"
#include "defs.h"
#include "pstat.h"

struct cpu cpus[NCPU];

struct proc proc[NPROC];
struct sleeplock memlocks[NPROC];

struct proc *initproc;

int nextpid = 1 ;
struct spinlock pid_lock;

extern void forkret(void);
static void freeproc(struct proc *p);

extern char trampoline[]; // trampoline.S

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void
proc_mapstacks(pagetable_t kpgtbl)
{
  struct proc *p;
  
  for(p = proc; p < &proc[NPROC]; p++) {
    char *pa = kalloc();
    if(pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int) (p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

// initialize the proc table.
void
procinit(void)
{
  struct proc *p;
  
  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock") ;
  int i = 0 ;
  for(p = proc; p < &proc[NPROC]; p++) {
      initlock(&p->lock, "proc");
      initsleeplock(&memlocks[i++], "mem");
      p->state = UNUSED;
      p->kstack = KSTACK((int) (p - proc));
  }
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int
cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu*
mycpu(void)
{
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc*
myproc(void)
{
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

int
allocpid()
{
  int pid;
  
  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}


// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;
  int i = 0 ;
  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state == UNUSED) {
      p->memlock = &memlocks[i] ;
      //printf("%d\n",i) ;
      goto found;
    } else {
      release(&p->lock);
    }
    i++ ;
  }
  return 0;

found:
  p->pid = allocpid();
  p->mem_id = p->pid ;
  p->state = USED;
  p->is_thread = 0 ;

  // Allocate a trapframe page.
  if((p->trapframe = (struct trapframe *)kalloc()) == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if(p->pagetable == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;
  p->inQ = 1 ;
  p->tickets_original = DEFAULT_TICKET_COUNT ;
  p->tickets_cur = DEFAULT_TICKET_COUNT ;
  p->time_slices = p->cur_slices = 0 ;

  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{
  if(p->trapframe)
    kfree((void*)p->trapframe);
  p->trapframe = 0;
  if(p->pagetable){
    if(p->is_thread){
      uvmunmap(p->pagetable, TRAMPOLINE, 1, 0, -1);
      uvmunmap(p->pagetable, TRAPFRAME, 1, 0, -1);
      if(p->sz > 0)
        uvmunmap(p->pagetable, 0, PGROUNDUP(p->sz) / PGSIZE, 1, p->mem_id);
      freewalk(p->pagetable);
    } else {
      proc_freepagetable(p->pagetable, p->sz, p->mem_id);
    }
  }
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->is_thread = 0 ;
  p->mem_id = 0 ;
  p->memlock = 0 ;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->state = UNUSED;
  // p->tickets_original = p->tickets_cur = 0 ;
  // p->cur_slices = p->time_slices = 0 ;
  // p->inQ = 1 ;
}

// Create a user page table for a given process, with no user memory,
// but with trampoline and trapframe pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if(pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if(mappages(pagetable, TRAMPOLINE, PGSIZE,
              (uint64)trampoline, PTE_R | PTE_X, 0, p->pid) < 0){
    uvmfree(pagetable, 0, -1);
    return 0;
  }

  // map the trapframe page just below the trampoline page, for
  // trampoline.S.
  if(mappages(pagetable, TRAPFRAME, PGSIZE,
              (uint64)(p->trapframe), PTE_R | PTE_W, 0, p->pid) < 0){
    uvmunmap(pagetable, TRAMPOLINE, 1, 0, -1);
    uvmfree(pagetable, 0, -1);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void
proc_freepagetable(pagetable_t pagetable, uint64 sz, int process_id)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0, -1);
  uvmunmap(pagetable, TRAPFRAME, 1, 0, -1);
  uvmfree(pagetable, sz, process_id);
}

// a user program that calls exec("/init")
// assembled from ../user/initcode.S
// od -t xC ../user/initcode
uchar initcode[] = {
  0x17, 0x05, 0x00, 0x00, 0x13, 0x05, 0x45, 0x02,
  0x97, 0x05, 0x00, 0x00, 0x93, 0x85, 0x35, 0x02,
  0x93, 0x08, 0x70, 0x00, 0x73, 0x00, 0x00, 0x00,
  0x93, 0x08, 0x20, 0x00, 0x73, 0x00, 0x00, 0x00,
  0xef, 0xf0, 0x9f, 0xff, 0x2f, 0x69, 0x6e, 0x69,
  0x74, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00
};

// Set up first user process.
void
userinit(void)
{
  struct proc *p;

  p = allocproc();
  release(&p->lock);
  initproc = p;
  
  // allocate one user page and copy initcode's instructions
  // and data into it.
  uvmfirst(p->pagetable, initcode, sizeof(initcode), p->pid);
  p->sz = PGSIZE;

  // prepare for the very first "return" from kernel to user.
  p->trapframe->epc = 0;      // user program counter
  p->trapframe->sp = PGSIZE;  // user stack pointer

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  acquire(&p->lock);
  p->state = RUNNABLE;

  release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint64 oldsz, newsz;
  struct proc *p = myproc();
  struct proc *pp;
  struct proc *mirrored[NPROC];
  int nmirrored = 0;

  acquiresleep(p->memlock);
  oldsz = p->sz;
  newsz = oldsz;

  if(n > 0){
    newsz = uvmalloc(p->pagetable, oldsz, oldsz + n, PTE_W, p->mem_id);
    if(newsz == 0){
      releasesleep(p->memlock);
      return -1;
    }
    for(pp = proc; pp < &proc[NPROC]; pp++){
      if(pp == p || pp->state == UNUSED || pp->mem_id != p->mem_id ||
         (!pp->is_thread && pp->pid != p->mem_id))
        continue;
      if(uvmallocmirror(p->pagetable, pp->pagetable, oldsz, newsz, p->mem_id) == 0){
        while(nmirrored > 0){
          pp = mirrored[--nmirrored];
          uvmdeallocmirror(pp->pagetable, newsz, oldsz, p->mem_id);
        }
        uvmdealloc(p->pagetable, newsz, oldsz, p->mem_id);
        releasesleep(p->memlock);
        return -1;
      }
      mirrored[nmirrored++] = pp;
    }
  } else if(n < 0){
    newsz = uvmdealloc(p->pagetable, oldsz, oldsz + n, p->mem_id);
    for(pp = proc; pp < &proc[NPROC]; pp++){
      if(pp == p || pp->state == UNUSED || pp->mem_id != p->mem_id ||
         (!pp->is_thread && pp->pid != p->mem_id))
        continue;
      uvmdeallocmirror(pp->pagetable, oldsz, newsz, p->mem_id);
    }
  }

  for(pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->state != UNUSED && pp->mem_id == p->mem_id &&
       (pp->is_thread || pp->pid == p->mem_id))
      pp->sz = newsz;
  }
  releasesleep(p->memlock);
  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int
fork(void)
{
  // printf("fork\n");
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }
  release(&np->lock);

  // Copy user memory from parent to child.
  if(uvmcopy(p->pagetable, np->pagetable, p->sz,p->mem_id, np->mem_id, &p->lock) < 0){
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  acquire(&np->lock);
  np->sz = p->sz;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;
  np->is_thread = 0 ;

  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  int tickets_original = 0;
  acquire(&p->lock);
  tickets_original = p->tickets_original;
  release(&p->lock);

  acquire(&np->lock);
  np->tickets_original = tickets_original;
  np->tickets_cur = tickets_original;
  np->time_slices = 0;
  np->cur_slices = 0 ;
  np->state = RUNNABLE;
  //np->inQ = p->inQ ;
  release(&np->lock);

  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void
reparent(struct proc *p)
{
  struct proc *pp;

  for(pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->parent == p){
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void
exit(int status)
{
  struct proc *p = myproc();

  if(p == initproc)
    panic("init exiting");

  // Close all open files.
  for(int fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd]){
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  acquire(&wait_lock);

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup(p->parent);
  
  acquire(&p->lock);

  p->xstate = status;
  p->state = ZOMBIE;

  release(&wait_lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(uint64 addr)
{
  struct proc *pp;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(pp = proc; pp < &proc[NPROC]; pp++){
      if(pp->parent == p){
        // make sure the child isn't still in exit() or swtch().
        acquire(&pp->lock);

        havekids = 1;
        if(pp->state == ZOMBIE){
          // Found one.
          pid = pp->pid;
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&pp->xstate,
                                  sizeof(pp->xstate),p->mem_id) < 0) {
            release(&pp->lock);
            release(&wait_lock);
            return -1;
          }
          release(&pp->lock);
          release(&wait_lock);
          freeproc(pp);
          return pid;
        }
        release(&pp->lock);
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || killed(p)){
      release(&wait_lock);
      return -1;
    }
    
    // Wait for a child to exit.
    sleep(p, &wait_lock);  //DOC: wait-sleep
  }
}


 void initialize(){
  struct proc *p ;
  for(p = proc ; p < &proc[NPROC] ; p++) {
    acquire(&p->lock) ;
    if(p->inQ == 1 && p->state != UNUSED) p->tickets_cur = p->tickets_original ;
    release(&p->lock) ;
  }
}
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
void scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  int rr_index = 0;

  acquire(&tickslock);
  uint64 start = ticks;
  release(&tickslock);

  c->proc = 0;
  for (;;)
  {
    // Avoid deadlock by ensuring that devices can interrupt.
    intr_on();

    acquire(&tickslock);
    uint64 cur = ticks;
    release(&tickslock);

    // Boost-Up if differences of ticks >= 64
    if(cur - start >= 10) {
      //printf("64") ;
      for(p = proc; p < &proc[NPROC]; p++) {
        p->inQ = 1;
        //p->tickets_cur = p->tickets_original ;//setting to original ticket_initial
      }
      start = cur ;
    }

    //taking the total_current_active_tickets!!
    int t = 0 ; //total_current_tickets variable
    for(p = proc ; p < &proc[NPROC] ; p++) {
      acquire(&p->lock) ;
      if(p->inQ == 1 && p->state == RUNNABLE && p->tickets_cur > 0) t+=p->tickets_cur ;
      release(&p->lock) ;
    }
    //printf("%d tickets\n",t);
    int taken = 0 ;

    if (t > 0) {
      int random = genrand(t) + 1 ;
      //printf("%d random\n",random) ;
      int prefix_sum = 0 ;

      for (p = proc ; p < &proc[NPROC] ; p++) {
        acquire(&p->lock) ;
        //printf("lock Aquired\n");
        if(p->inQ == 1 && p->state == RUNNABLE && p->tickets_cur > 0) {
          prefix_sum += p->tickets_cur ;
          if (prefix_sum >= random) { //The random number is found in between a range
            //need to release the lock if already found
            if(p->state == RUNNABLE) {
              p->state = RUNNING ;
              c->proc = p ; //set the current process to p
              p->tickets_cur-- ;
              taken = 1;
              //p->time_slices++ ;
              swtch(&c->context, &p->context) ; //save context of cpu and load the new context of process p
              c->proc = 0 ;

              release(&p->lock) ;
              break ;
            }
            // else if(p->state==ZOMBIE){
            //   p->inQ = 2 ;
            // }
          }
        }
        release(&p->lock) ;
        //printf("LOck released\n") ;
      }
    }
    if(taken == 0)
    {
      //printf("Entering here") ;
      for (int offset = 0; offset < NPROC; offset++) {
        int index = (rr_index + offset) % NPROC;
        p = &proc[index];
        acquire(&p->lock);
        if(p->state == RUNNABLE) {

          // Switch to chosen process.  It is the process's job
          // to release its lock and then reacquire it
          // before jumping back to us.
          p->state = RUNNING;
          c->proc = p;
          //p->time_slices+=2 ;

          swtch(&c->context, &p->context) ;
          // Process is done running for now.
          // It should have changed its p->state before coming back.
          c->proc = 0 ;
          release(&p->lock) ;
          rr_index = (index + 1) % NPROC;
          break ;
        }
        release(&p->lock);
      }
      initialize() ;
    }
  }
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&p->lock))
    panic("sched p->lock");
  if(mycpu()->noff != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(intr_get())
    panic("sched interruptible");
  if(p->state == SLEEPING) {
    //printf("\nSLEEPING %d %d\n", p->pid,p->inQ) ;
    // acquire(&p->lock) ;
    p->inQ = 1 ;
    //release(&p->lock) ;
  }
  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->cur_slices+=1 ;
  p->time_slices+=1 ;
  //printf("\n%d  pid in queue%d\n",p->pid,p->inQ) ;
  if(p->inQ == 1 && p->cur_slices >= TIME_LIMIT_1) {
    //printf("%d id with process switching to queue2 %d\n\n",p->pid,p->time_slices) ;
    p->state = RUNNABLE ;
    p->inQ = 2 ;
    p->cur_slices = 0 ;
    sched() ;
  }
  else if(p->inQ == 2){
    if(p->cur_slices >= TIME_LIMIT_2){
      p->state = RUNNABLE ;
      p->cur_slices = 0 ;
      sched() ;
    }
    else {
      p->state =RUNNING ;
      //printf("RUNNING\n") ;
    }
  }
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void)
{
  static int first = 1;

  // Still holding p->lock from scheduler.
  release(&myproc()->lock);

  if (first) {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    first = 0;
    fsinit(ROOTDEV);
  }

  usertrapret();
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  // printf("here\n");
  struct proc *p = myproc();
  
  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.

  acquire(&p->lock);  //DOC: sleeplock1
  release(lk);

  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  release(&p->lock);
  acquire(lk);
}

// Wake up all processes sleeping on chan.
// Must be called without any p->lock.
void
wakeup(void *chan)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    if(p != myproc()){
      acquire(&p->lock);
      if(p->state == SLEEPING && p->chan == chan)
        p->state = RUNNABLE;
      release(&p->lock);
    }
  }
}

int
create_thread(void(*fcn)(void*) , void* arg , void* stack) {
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }
  // Mirroring can sleep during swap I/O. wakeup() scans every proc lock.
  release(&np->lock);

  // Share the parent's user mappings and swap identity.
  acquiresleep(p->memlock);
  np->mem_id = p->mem_id;
  if(uvmmirror(p->pagetable, np->pagetable, 0, p->sz, p->mem_id) < 0){
    releasesleep(p->memlock);
    acquire(&np->lock);
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->memlock = p->memlock;
  np->sz = p->sz;
  np->is_thread = 1;
  releasesleep(p->memlock);
  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  //taking the arguments in a0 resgister
  np->trapframe->a0 = (uint64)arg ;
  np->trapframe->epc = (uint64) fcn ;
  np->trapframe->sp = (1 << 12) + (uint64) stack ;
  np->trapframe->sp-= np->trapframe->sp % 16 ; // same from exec.c
  np->trapframe->ra = 0xffffffff ;

  // increment reference counts on open file descriptors.
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->lock);
  np->state = RUNNABLE;
  release(&np->lock);

  return pid;
}

int
join_thread(int id) {
  struct proc *pp;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(pp = proc; pp < &proc[NPROC]; pp++){
      if(pp->parent == p && pp->pid == id && pp->mem_id == p->mem_id){
        // make sure the child isn't still in exit() or swtch().
        acquire(&pp->lock);

        havekids = 1;
        if(pp->state == ZOMBIE){
          // Found one.
          pid = pp->pid ;
          pp->parent = 0;
          release(&pp->lock);
          release(&wait_lock);
          acquiresleep(p->memlock);
          freeproc(pp);
          releasesleep(p->memlock);
          return pid ;
        }
        release(&pp->lock);
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || killed(p)){
      release(&wait_lock);
      return -1;
    }

    // Wait for a child to exit.
    sleep(p, &wait_lock);  //DOC: wait-sleep
  }
}

void
exit_thread(void) {
  exit(0) ;
}

void new_sleep(uint64 locked) {

    struct proc* p = myproc() ;
    acquire(&p->lock) ;
    uint64 va0, pa0;
    va0 = PGROUNDDOWN(locked);
    pa0 = walkaddr(p->pagetable, va0, p->mem_id);
    // if(pa0 == 0)
    //   return -1;
    __sync_synchronize();
    __sync_lock_release((uint8*)(pa0 + (locked - va0)));

    p->state = SLEEPING ;
    sched() ;

    release(&p->lock) ;
}

void new_wakeup(int id) {
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    if(p != myproc()){
      acquire(&p->lock);
      if(p->state == SLEEPING && p->pid == id) {
        p->state = RUNNABLE;
      }
      release(&p->lock);
    }
  }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int
kill(int pid)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid){
      p->killed = 1;
      if(p->state == SLEEPING){
        // Wake process from sleep().
        p->state = RUNNABLE;
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

void
killmemgroup(int mem_id)
{
  for(struct proc *p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->state != UNUSED && p->mem_id == mem_id){
      p->killed = 1;
      if(p->state == SLEEPING)
        p->state = RUNNABLE;
    }
    release(&p->lock);
  }
}

void
setkilled(struct proc *p)
{
  acquire(&p->lock);
  p->killed = 1;
  release(&p->lock);
}

int
killed(struct proc *p)
{
  int k;
  
  acquire(&p->lock);
  k = p->killed;
  release(&p->lock);
  return k;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if(user_dst){
    return copyout(p->pagetable, dst, src, len,p->mem_id);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  // printf("%d\n",user_src);
  struct proc *p = myproc();
  if(user_src){
    return copyin(p->pagetable, dst, src, len, p->mem_id);
  } else {
    memmove(dst, (char*)src, len);
    return 0;
  }
}

// Print a process listing to console.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [USED]      "used",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  struct proc *p;
  char *state;

  printf("\n");
  for(p = proc; p < &proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d %s %s", p->pid, state, p->name);
    printf("\n");
  }
}

int
settickets(int ticket)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->tickets_original = ticket;
  p->tickets_cur = ticket;
  p->time_slices = 0;
  release(&p->lock);
  return 0;
}

int
getpinfo(uint64 addr)
{
  int i = 0;
  struct proc *mp = myproc();
  struct proc *p;
  struct pstat pst;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    pst.pid[i] = p->pid;
    pst.inuse[i] = p->state == UNUSED ? -1 :
                   (p->state == RUNNABLE || p->state == RUNNING);
    pst.tickets_original[i] = p->tickets_original;
    pst.tickets_current[i] = p->tickets_cur;
    pst.time_slices[i] = p->time_slices;
    pst.inQ[i] = p->inQ;
    release(&p->lock);
    i++;
  }
  if(copyout(mp->pagetable, addr, (char *)&pst, sizeof(pst), mp->mem_id) < 0)
    return -1;
  return 0;
}

void process_page_count(){
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    // int sp = 0;
    // int l = 0;
    if(p->state != UNUSED){
      Count(p->sz,p->mem_id,p->pagetable) ;
      //pg_count(p->pid) ;
      //pageCount(p->pagetable, 0,&l, &sp);
      //printf("process_id: %d livePage :%d swappedPage: %d\n", p->pid,l,sp);
    }
    release(&p->lock);
  }
}
