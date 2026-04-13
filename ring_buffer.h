#ifndef RING_BUFFER_H
#define RING_BUFFER_H

#include <inttypes.h>
#include <stdint.h>

#define MAX_MSG_LEN 64

typedef struct {
    uint64_t timestamp;
    int severity;        /* 0=DEBUG, 1=INFO, 2=WARN, 3=ERROR */
    char message[MAX_MSG_LEN];
} event_t;

typedef struct {
    event_t *buf;
    int capacity;
    int head;            /* next write position */
    int count;           /* number of valid entries (max = capacity) */
} ring_buf_t;

ring_buf_t *ring_buf_create(int capacity);
int ring_buf_push(ring_buf_t *rb, const event_t *event);
int ring_buf_pop(ring_buf_t *rb, event_t *out);
int ring_buf_dump(ring_buf_t *rb, int min_severity);
void ring_buf_destroy(ring_buf_t *rb);

#endif /* RING_BUFFER_H */
