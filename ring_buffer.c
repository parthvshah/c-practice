#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "ring_buffer.h"

ring_buf_t *ring_buf_create(int capacity) {
    if (capacity <= 0)
        return NULL;

    ring_buf_t *rb = malloc(sizeof(ring_buf_t));
    if (!rb)
        return NULL;

    rb->buf = calloc(capacity, sizeof(event_t));
    if (!rb->buf) {
        free(rb);
        return NULL;
    }

    rb->capacity = capacity;
    rb->head = 0;
    rb->count = 0;

    return rb;
}

int ring_buf_push(ring_buf_t *rb, const event_t *event) {
    if (!rb || !event)
        return -1;

    memcpy(&rb->buf[rb->head], event, sizeof(event_t));
    rb->head = (rb->head + 1) % rb->capacity;

    if (rb->count < rb->capacity)
        rb->count++;

    return 0;
}

int ring_buf_pop(ring_buf_t *rb, event_t *out) {
    if (!rb || !out || rb->count == 0)
        return -1;

    int tail = (rb->head - rb->count + rb->capacity) % rb->capacity;
    memcpy(out, &rb->buf[tail], sizeof(event_t));
    rb->count--;

    return 0;
}

int ring_buf_dump(ring_buf_t *rb, int min_severity) {
    if (!rb)
        return 0;

    static const char *labels[] = { "DEBUG", "INFO", "WARN", "ERROR" };

    int printed = 0;
    int tail = (rb->head - rb->count + rb->capacity) % rb->capacity;

    for (int i = 0; i < rb->count; i++) {
        int idx = (tail + i) % rb->capacity;
        event_t *e = &rb->buf[idx];

        if (e->severity >= min_severity) {
            const char *label = (e->severity >= 0 && e->severity <= 3)
                                 ? labels[e->severity] : "UNK";
            printf("[%" PRIu64 "] %s: %s\n", e->timestamp, label, e->message);
            printed++;
        }
    }
    return printed;
}

void ring_buf_destroy(ring_buf_t *rb) {
    if (!rb)
        return;
    free(rb->buf);
    free(rb);
}
