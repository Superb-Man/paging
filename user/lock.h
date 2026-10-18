// Mutual exclusion spin locks.
#pragma once

#include "kernel/types.h"
#include "kernel/riscv.h"
#include "user/user.h"

struct thread_spinlock {
  uint8 locked;       // Is the lock held?
  int pid ;
};

int holding(struct thread_spinlock *lk) ;

void
thread_spin_init(struct thread_spinlock *lk)
{
  lk->locked = 0;
  lk->pid = 0 ;
}

// Acquire the lock.
// Loops (spins) until the lock is acquired.
void
thread_spin_lock(struct thread_spinlock *lk)
{
    // if(!holding(lk))
    //     return ;

    // On RISC-V, sync_lock_test_and_set turns into an atomic swap:
    //   a5 = 1
    //   s1 = &lk->locked
    //   amoswap.w.aq a5, a5, (s1)
    while(__sync_lock_test_and_set(&lk->locked, 1) != 0) ;

    // Tell the C compiler and the processor to not move loads or stores
    // past this point, to ensure that the critical section's memory
    // references happen strictly after the lock is acquired.
    // On RISC-V, this emits a fence instruction.
    __sync_synchronize();

    // Record info about lock acquisition for holding() and debugging.
    lk->pid = getpid() ;
}

// Release the lock.
void
thread_spin_unlock(struct thread_spinlock *lk)
{
//   if(!holding(lk)) 
//     return ;

  lk->pid = 0 ;

  // Tell the C compiler and the CPU to not move loads or stores
  // past this point, to ensure that all the stores in the critical
  // section are visible to other CPUs before the lock is released,
  // and that loads in the critical section occur strictly before
  // the lock is released.
  // On RISC-V, this emits a fence instruction.
  __sync_synchronize();

  // Release the lock, equivalent to lk->locked = 0.
  // This code doesn't use a C assignment, since the C standard
  // implies that an assignment might be implemented with
  // multiple store instructions.
  // On RISC-V, sync_lock_release turns into an atomic swap:
  //   s1 = &lk->locked
  //   amoswap.w zero, zero, (s1)
  __sync_lock_release(&lk->locked);
}

// Check whether this cpu is holding the lock.
// Interrupts must be off.
int
holding(struct thread_spinlock *lk)
{
  return lk->locked && lk->pid == getpid() ;
}

struct thread_mutex{
    uint8 locked ;
    int pid ;
} ;

int holding2(struct thread_mutex *lk) {
    return lk->locked && lk->pid == getpid() ;
}

void
thread_mutex_init(struct thread_mutex *lk)
{
  lk->locked = 0;
  lk->pid = 0;
}

void thread_mutex_lock(struct thread_mutex *lk) {
    while(__sync_lock_test_and_set(&lk->locked, 1) != 0) {
        //sleep(1) ;
        yield(0) ;
    }

    // Tell the C compiler and the processor to not move loads or stores
    // past this point, to ensure that the critical section's memory
    // references happen strictly after the lock is acquired.
    // On RISC-V, this emits a fence instruction.
    __sync_synchronize();

    // Record info about lock acquisition for holding() and debugging.
    //lk->pid = getpid() ;
}

void thread_mutex_unlock(struct thread_mutex *lk) {
    lk->pid = 0 ;

    // Tell the C compiler and the CPU to not move loads or stores
    // past this point, to ensure that all the stores in the critical
    // section are visible to other CPUs before the lock is released,
    // and that loads in the critical section occur strictly before
    // the lock is released.
    // On RISC-V, this emits a fence instruction.
    __sync_synchronize();

    // Release the lock, equivalent to lk->locked = 0.
    // This code doesn't use a C assignment, since the C standard
    // implies that an assignment might be implemented with
    // multiple store instructions.
    // On RISC-V, sync_lock_release turns into an atomic swap:
    //   s1 = &lk->locked
    //   amoswap.w zero, zero, (s1)
    __sync_lock_release(&lk->locked);
}
