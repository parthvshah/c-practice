#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct bounded_queue {
    void   **buf;
    size_t   cap;
    size_t   head;     // pop from here
    size_t   tail;     // push to here
    size_t   count;
    pthread_mutex_t mtx;
    pthread_cond_t  not_full;
    pthread_cond_t  not_empty;
} bounded_queue_t;

bounded_queue_t *bq_create(size_t capacity) {
    bounded_queue_t *q = calloc(1, sizeof(*q));
    if (!q)
        return NULL;
    q->buf = calloc(capacity, sizeof(void *));
    if (!q->buf) { 
        free(q); 
        return NULL; 
    }
    q->cap = capacity;
    pthread_mutex_init(&q->mtx, NULL);
    pthread_cond_init(&q->not_full, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    return q;
}

void bq_destroy(bounded_queue_t *q) {
    pthread_mutex_destroy(&q->mtx);
    pthread_cond_destroy(&q->not_full);
    pthread_cond_destroy(&q->not_empty);
    free(q->buf);
    free(q);
}

void bq_push(bounded_queue_t *q, void *item) {
    pthread_mutex_lock(&q->mtx);
        while (q->count == q->cap) {
            pthread_cond_wait(&q->not_full, &q->mtx);   // while loop handles spurious wakeups
        }
        q->buf[q->tail] = item;
        q->tail = (q->tail + 1) % q->cap;
        q->count++;
    pthread_mutex_unlock(&q->mtx);
    pthread_cond_signal(&q->not_empty);             // signal one waiter, not broadcast
}

void *bq_pop(bounded_queue_t *q) {
    pthread_mutex_lock(&q->mtx);
        while (q->count == 0) {
            pthread_cond_wait(&q->not_empty, &q->mtx);
        }
        void *item = q->buf[q->head];
        q->head = (q->head + 1) % q->cap;
        q->count--;
    pthread_mutex_unlock(&q->mtx);
    pthread_cond_signal(&q->not_full);

    return item;
}