#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "user/lock.h"

int* ara ;
struct thread_spinlock lock ;
struct thread_mutex mlock ;

struct balance {
    char name[32];
    int amount;
};

volatile int total_balance = 0;

volatile unsigned int delay (unsigned int d) {
   unsigned int i; 
   for (i = 0; i < d; i++) {
       __asm volatile( "nop" ::: );
   }

   return i;   
}

void do_work(void *arg){
    int i; 
    int old;
   
    struct balance *b = (struct balance*) arg; 
    thread_mutex_lock(&mlock) ;
    printf( "Starting do_work: s:%s\n", b->name);
    thread_mutex_unlock(&mlock) ;

    for (i = 0; i < b->amount; i++) { 
        // lock and mlock will be implemented by you.
         //thread_spin_lock(&lock);
         thread_mutex_lock(&mlock);
         old = total_balance;
         //delay(100000);
	     //if(old != total_balance)  
         //printf("we will miss an update.%s %d old: %d total_balance: %d\n",b->name, i,old, total_balance);
         total_balance = old + 1;
         //printf("%d\n",ara[i]) ;
         ara[i] = i ;
         //thread_spin_unlock(&lock);
         thread_mutex_unlock(&mlock);
         //sleep(1) ;
         //

    }
    
    thread_mutex_lock(&mlock) ;
    printf( "Done s:%s\n", b->name) ;
    thread_mutex_unlock(&mlock) ;

    exit_thread();
    return;
}

int main(int argc, char *argv[]) {

  struct balance b1 = {"b1", 3200};
  struct balance b2 = {"b2", 2800};
 
  void *s1, *s2;
  int thread1, thread2, r1 = 1, r2 =1 ;

  s1 = malloc(4096); // 4096 is the PGSIZE defined in kernel/riscv.h
  s2 = malloc(4096);

  thread_spin_init(&lock) ;
  thread_mutex_init(&mlock) ;

//   ara = malloc(10000 * sizeof(int));
//   if(ara == 0){
//     printf("ara allocation failed\n");
//     exit(1);
//   }
//   for(int i = 0; i < 10000; i++)
//     ara[i] = 5;

  thread1 = create_thread(do_work, (void*)&b1, s1);
  thread2 = create_thread(do_work, (void*)&b2, s2); 

  ara = malloc(10000 * sizeof(int));
  if(ara == 0){
    printf("ara allocation failed\n");
    exit(1);
  }

  for(int i = 0; i < 10000; i++)
    ara[i] = 5;

  r1 = join_thread(thread1);
  r2 = join_thread(thread2);
  free(ara) ;
  
  printf("Threads finished: (%d):%d, (%d):%d, shared balance:%d\n", 
      thread1, r1, thread2, r2, total_balance);

  exit(0) ;
}
