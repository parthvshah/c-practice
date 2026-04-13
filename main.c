#include <stdio.h>
#include <inttypes.h>

#include "ring_buffer.h"
#include "parse_mem.h"

static void test_ring_buffer(void) {
    printf("=== ring_buffer ===\n");

    ring_buf_t *rb = ring_buf_create(3);
    if (!rb) {
        fprintf(stderr, "Failed to create ring buffer\n");
        return;
    }

    /* Push 5 events into a buffer of capacity 3. The first two should
     * be overwritten by events 4 and 5. */
    for (int i = 1; i <= 5; i++) {
        event_t e = {
            .timestamp = 1000 + i,
            .severity = i % 4,
        };
        snprintf(e.message, MAX_MSG_LEN, "Node %d health check", i);
        ring_buf_push(rb, &e);
    }

    printf("Dump all (expect events 3, 4, 5):\n");
    ring_buf_dump(rb, 0);

    printf("\nPop oldest:\n");
    event_t popped;
    if (ring_buf_pop(rb, &popped) == 0)
        printf("  Popped: [%" PRIu64 "] %s\n", popped.timestamp, popped.message);

    printf("\nDump remaining (expect events 4, 5):\n");
    ring_buf_dump(rb, 0);

    ring_buf_destroy(rb);
    printf("\nAll memory freed. Done.\n");
}

static void test_parse_mem(void) {
    printf("\n=== parse_mem ===\n");

    const char *test_input =
        "MemTotal:       32768000 kB\n"
        "MemFree:         1024000 kB\n"
        "MemAvailable:   16384000 kB\n"
        "Buffers:          512000 kB\n"
        "Cached:          8192000 kB\n"
        "SwapTotal:       4096000 kB\n"
        "SwapFree:        4096000 kB\n";

    meminfo_entry_t entries[16];
    int count = parse_meminfo(test_input, entries, 16);
    printf("Parsed %d entries:\n", count);
    for (int i = 0; i < count; i++)
        printf("  %-20s %" PRIu64 " kB\n", entries[i].key, entries[i].value_kb);

    printf("\n");
    print_memory_summary(test_input);

    printf("\nEmpty input test:\n");
    count = parse_meminfo("", entries, 16);
    printf("  Parsed %d entries (expected 0)\n", count);

    printf("\nMalformed input test:\n");
    count = parse_meminfo("NotAValidLine\nAlsoBad\nGood: 42 kB\n", entries, 16);
    printf("  Parsed %d entries (expected 1)\n", count);
}

int main(void) {
    test_ring_buffer();
    test_parse_mem();
    return 0;
}
