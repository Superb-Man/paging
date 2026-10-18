#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "pstat.h"
uint64
sys_exit(void)
{
  int n;
  argint(0, &n);
  exit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return fork();
}

uint64
sys_wait(void)
{
  uint64 p;
  argaddr(0, &p);
  return wait(p);
}

uint64
sys_sbrk(void)
{
  uint64 addr;
  int n;

  argint(0, &n);
  addr = myproc()->sz;
  if(growproc(n) < 0)
    return -1;
  return addr;
}

uint64
sys_sleep(void)
{
  int n;
  uint ticks0;

  argint(0, &n);
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(killed(myproc())){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_kill(void)
{
  int pid;

  argint(0, &pid);
  return kill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

uint64
sys_create_thread(void) {
  uint64 fcn ;
  uint64 arg ;
  uint64 stack ;
  argaddr(0, &fcn) ; 
  argaddr(1 , &arg) ;
  argaddr(2, &stack) ;

  return create_thread((void*) fcn, (void*) arg, (void*) stack) ;
}

uint64
sys_exit_thread(void) {
  exit_thread() ;
  return 0 ;
}

uint64
sys_join_thread(void) {
  int id ;
  argint(0,&id) ;
  return join_thread(id) ;
}

uint64
sys_yield(void) {
  int x ;
  argint(0,&x) ;
  yield() ;
  return 0 ;
}

uint64
sys_release_sleep(void) {
  uint64 addr ;
  argaddr(0, &addr) ;
  new_sleep(addr) ;

  return 0 ;
}

uint64
sys_wakeup(void) {
  int id ;
  argint(0, &id) ;
  new_wakeup(id) ;
  return 0 ;
}


// sets the ticket count
uint64
sys_settickets(void)
{
  int ticketCount;
  argint(0,&ticketCount);
  if( ticketCount < 0)
    return -1;
  else if( ticketCount == 0 ){
    return settickets(1);
  }
  else{
    return settickets(ticketCount);
  }

  return 0;
}
// gets the ticket count
uint64
sys_getpinfo(void)
{
  uint64 addr;
  argaddr(0, &addr);
  return getpinfo(addr);

}

uint64 sys_pagestats(void){
  int a;
  // int c = 0;
  argint(0, &a);
  int c = 0;
  printf("===================== PAGESTATS ==================\n");
  if(!a) {
    return freePageCount() ;
  }
  process_page_count() ;
  pageCountInfo() ;
  printf("__________________________________________________\n");
  return c;
}