#include "kernel/types.h"
struct queue{
	int arr[16];
	int front ;
	int rear ;
	int size ;
} ;
void init_queue(struct queue* q) {
	q->front = q->size = q->rear = 0 ;
}

void push(struct queue* q , int x)
{
	q->arr[q->rear] = x;
	q->rear = (q->rear+1)%16;
	q->size++;
}
int front(struct queue* q) {
	if(q->size == 0) return -1;
	return q->arr[q->front];
}
void pop(struct queue *q)
{
    if(q->size != 0) 
	{
		q->front = (q->front+1)%16;
		q->size--;
	}
}