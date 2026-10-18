#pragma once

#include "kernel/types.h"
#include "user/lock.h"
#include "user/queue.h"
#include "user/user.h"
#define NTHREAD 64 

#include "kernel/types.h"
struct tqueue{
	int arr[64];
	int front ;
	int rear ;
	int size ;
} ;
void init_tqueue(struct tqueue* q) {
	q->front = q->size = q->rear = 0 ;
}

void tpush(struct tqueue* q , int x)
{
	q->arr[q->rear] = x;
	q->rear = (q->rear+1)%NTHREAD;
	q->size++;
}
int tfront(struct tqueue* q) {
	if(q->size == 0) return -1;
	return q->arr[q->front];
}
void tpop(struct tqueue *q)
{
    if(q->size != 0) 
	{
		q->front = (q->front+1)%NTHREAD;
		q->size--;
	}
}
struct cond_var{
    struct thread_mutex qlock ;
    struct tqueue waiting_queue ;
};

void init_cond_var(struct cond_var* cv,struct thread_mutex* mutex) {
    thread_mutex_init(&cv->qlock) ;
    thread_mutex_init(mutex) ;
    init_tqueue(&cv->waiting_queue) ;
}

void await(struct cond_var* cv ,struct thread_mutex* mutex) {
    thread_mutex_lock(&cv->qlock) ;
    tpush(&cv->waiting_queue,getpid()) ;
    thread_mutex_unlock(&cv->qlock) ;

    release_sleep(&mutex->locked) ;

    thread_mutex_lock(mutex) ;
}

void notify(struct cond_var* cv) {
    int tid ;

    thread_mutex_lock(&cv->qlock); /* protect the queue */
    tid = tfront(&cv->waiting_queue) ;
    tpop(&cv->waiting_queue) ;
    thread_mutex_unlock(&cv->qlock) ;

    if (tid > 0)
        wakeup(tid) ;

    return;
}

void notifyall(struct cond_var* cv) {
    int tid;

    thread_mutex_lock(&cv->qlock); /* protect the queue */
    while (cv->waiting_queue.size != 0) /* queue is not empty */
    {
      tid = tfront(&cv->waiting_queue); /* wake one */
      tpop(&cv->waiting_queue) ;
      wakeup(tid); /* Make it runnable */
    }
    thread_mutex_unlock(&cv->qlock) ; /* done with the queue */
  }