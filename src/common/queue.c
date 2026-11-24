#include "common/queue.h"

#include <stdlib.h>

int queue_init(queue_t *q, size_t capacity)
{
    q->items = calloc(capacity, sizeof(void *));
    if (!q->items)
        return -1;
    q->cap = capacity;
    q->head = 0;
    q->count = 0;
    q->closed = false;
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    return 0;
}

void queue_destroy(queue_t *q)
{
    pthread_cond_destroy(&q->not_empty);
    pthread_mutex_destroy(&q->lock);
    free(q->items);
    q->items = NULL;
}

bool queue_try_push(queue_t *q, void *item)
{
    pthread_mutex_lock(&q->lock);
    if (q->closed || q->count == q->cap) {
        pthread_mutex_unlock(&q->lock);
        return false;
    }
    q->items[(q->head + q->count) % q->cap] = item;
    q->count++;
    /* Signal while holding the lock: one waiting worker wakes per job. */
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->lock);
    return true;
}

void *queue_pop(queue_t *q)
{
    pthread_mutex_lock(&q->lock);
    /* A while loop, not an if: wake-ups can be spurious, and another worker may have
     * taken the item between the signal and this thread running. */
    while (q->count == 0 && !q->closed)
        pthread_cond_wait(&q->not_empty, &q->lock);
    void *item = NULL;
    if (q->count > 0) {
        item = q->items[q->head];
        q->head = (q->head + 1) % q->cap;
        q->count--;
    }
    pthread_mutex_unlock(&q->lock);
    return item;
}

void queue_close(queue_t *q)
{
    pthread_mutex_lock(&q->lock);
    q->closed = true;
    pthread_cond_broadcast(&q->not_empty);
    pthread_mutex_unlock(&q->lock);
}

size_t queue_depth(queue_t *q)
{
    pthread_mutex_lock(&q->lock);
    size_t n = q->count;
    pthread_mutex_unlock(&q->lock);
    return n;
}
