/*
 * queue.h - bounded multi-producer / multi-consumer queue of pointers.
 *
 * A fixed ring buffer guarded by one mutex and one condition variable. The reactor
 * pushes with queue_try_push(), which never blocks: when the ring is full it fails
 * immediately and the caller answers "busy" instead of letting memory grow without
 * bound (backpressure). Workers block in queue_pop() until a job arrives or the
 * queue is closed for shutdown.
 */
#ifndef OF_QUEUE_H
#define OF_QUEUE_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    void          **items;
    size_t          cap;
    size_t          head;   /* next to pop */
    size_t          count;
    bool            closed;
    pthread_mutex_t lock;
    pthread_cond_t  not_empty;
} queue_t;

int  queue_init(queue_t *q, size_t capacity);
void queue_destroy(queue_t *q);

/* false when full or closed; never blocks. */
bool queue_try_push(queue_t *q, void *item);

/* Blocks until an item is available. NULL once the queue is closed AND drained. */
void *queue_pop(queue_t *q);

/* Wake every waiting consumer; pops still return the remaining items first. */
void queue_close(queue_t *q);

size_t queue_depth(queue_t *q);

#endif
