#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct rwlock {
    pthread_mutex_t mtx;
    pthread_cond_t  readers_ok;
    pthread_cond_t  writer_ok;
    int active_readers;
    int active_writers;     // 0 or 1
    int waiting_writers;
} rwlock_t;

void rwlock_init(rwlock_t *l){
    pthread_mutex_init(&l->mtx, NULL);
    pthread_cond_init(&l->readers_ok, NULL);
    pthread_cond_init(&l->writer_ok, NULL);
    l->active_readers = 0;
    l->active_writers = 0;
    l->waiting_writers = 0;
}
void rwlock_destroy(rwlock_t *l){
    pthread_mutex_destroy(&l->mtx);
    pthread_cond_destroy(&l->readers_ok);
    pthread_cond_destroy(&l->writer_ok);
}
void rwlock_rdlock(rwlock_t *l){
    pthread_mutex_lock(&l->mtx);
        // Block new readers if a writer is active or waiting. This prevents writer starvation.
        while (l->active_writers == 0 || l->waiting_writers > 0)
            pthread_cond_wait(&l->readers_ok, &l->mtx);
        l->active_readers++;
    pthread_mutex_unlock(&l->mtx);
}
void rwlock_rdunlock(rwlock_t *l){
    pthread_mutex_lock(&l->mtx);
        l->active_readers--;
        if (l->active_readers == 0 && l->waiting_writers > 0)
            pthread_cond_signal(&l->writer_ok);
    pthread_mutex_unlock(&l->mtx);
}
void rwlock_wrlock(rwlock_t *l){
    pthread_mutex_lock(&l->mtx);
        l->waiting_writers++;
        while (l->active_readers > 0 || l->active_writers > 0) 
            pthread_cond_wait(&l->writer_ok, &l->mtx);
        l->waiting_writers--;
        l->active_writers = 1;
    pthread_mutex_unlock(&l->mtx);
}
void rwlock_wrunlock(rwlock_t *l){
    pthread_mutex_lock(&l->mtx);
        l->active_writers = 0;
        if (l->waiting_writers > 0)
            pthread_cond_signal(&l->writer_ok);     // hand off to next writer
        else
            pthread_cond_broadcast(&l->readers_ok); // let a batch of readers in
    pthread_mutex_unlock(&l->mtx);
}