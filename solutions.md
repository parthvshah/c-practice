# CoreWeave Kernel Engineer: Full Reference Solutions

**Companion to:** `coreweave_kernel_c_practice_problems.md`
**Purpose:** Study these after attempting each problem yourself. Each solution includes the code, line-by-line design rationale, and interview talking points you can weave into your narration.

---

## Solution 1: Ring Buffer for Kernel Event Log

### Design Choices

The core decision in a ring buffer is how to track the read and write positions. There are two common approaches: (1) store `head` and `tail` indices, or (2) store `head` (write position) and `count`. I use the head/count approach because it makes the "is full" and "is empty" checks unambiguous without sacrificing a slot or using a separate flag.

When the buffer is full and a new event arrives, the oldest entry is silently overwritten. This is the standard behavior in kernel trace buffers. The alternative, blocking or returning an error, would be appropriate for a bounded queue but not for a diagnostic logger where dropping old data is acceptable.

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define MAX_MSG_LEN 64

typedef struct {
    uint64_t timestamp;
    int severity;
    char message[MAX_MSG_LEN];
} event_t;

typedef struct {
    event_t *buf;
    int capacity;
    int head;      /* next write position */
    int count;     /* number of valid entries, capped at capacity */
} ring_buf_t;

ring_buf_t *ring_buf_create(int capacity)
{
    /*
     * Reject nonsensical capacities. A capacity of 0 would make the
     * buffer permanently empty, which is never useful. Negative values
     * indicate a caller bug.
     */
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

int ring_buf_push(ring_buf_t *rb, const event_t *event)
{
    if (!rb || !event)
        return -1;

    /*
     * Copy the event into the current head position. If the buffer is
     * already full, this overwrites the oldest entry. We use memcpy
     * rather than struct assignment for clarity about what is happening
     * at the byte level, though both are correct here.
     */
    memcpy(&rb->buf[rb->head], event, sizeof(event_t));

    /* Advance head with wraparound. */
    rb->head = (rb->head + 1) % rb->capacity;

    /* Count grows until it hits capacity, then stays there. */
    if (rb->count < rb->capacity)
        rb->count++;

    return 0;
}

int ring_buf_pop(ring_buf_t *rb, event_t *out)
{
    if (!rb || !out || rb->count == 0)
        return -1;

    /*
     * The oldest entry (tail) is derived from head and count.
     * tail = (head - count + capacity) % capacity
     *
     * This avoids storing a separate tail index. The modular arithmetic
     * ensures correctness even when head has wrapped around.
     */
    int tail = (rb->head - rb->count + rb->capacity) % rb->capacity;

    memcpy(out, &rb->buf[tail], sizeof(event_t));
    rb->count--;

    return 0;
}

int ring_buf_dump(ring_buf_t *rb, int min_severity)
{
    if (!rb)
        return 0;

    int printed = 0;
    int tail = (rb->head - rb->count + rb->capacity) % rb->capacity;

    for (int i = 0; i < rb->count; i++) {
        int idx = (tail + i) % rb->capacity;
        event_t *e = &rb->buf[idx];

        if (e->severity >= min_severity) {
            const char *sev_str[] = {"DEBUG", "INFO", "WARN", "ERROR"};
            const char *label = (e->severity >= 0 && e->severity <= 3)
                                ? sev_str[e->severity] : "UNKNOWN";
            printf("[%lu] %s: %s\n", e->timestamp, label, e->message);
            printed++;
        }
    }

    return printed;
}

void ring_buf_destroy(ring_buf_t *rb)
{
    if (!rb)
        return;
    free(rb->buf);
    free(rb);
}

/* ---- Test harness ---- */

int main(void)
{
    ring_buf_t *rb = ring_buf_create(3);
    if (!rb) {
        fprintf(stderr, "Failed to create ring buffer\n");
        return 1;
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
        printf("  Popped: [%lu] %s\n", popped.timestamp, popped.message);

    printf("\nDump remaining (expect events 4, 5):\n");
    ring_buf_dump(rb, 0);

    ring_buf_destroy(rb);
    printf("\nAll memory freed. Done.\n");
    return 0;
}
```

### Interview Talking Points

- "I chose head/count over head/tail because it eliminates the ambiguity of head==tail meaning either full or empty. The tradeoff is one extra branch in push, which is negligible."
- "In a production kernel context, this would be per-CPU with no locking needed for the common single-producer path. For multi-producer, I would use `atomic_cmpxchg` on the head index to implement a lock-free push."
- "The `calloc` in create is intentional. Zero-initializing the buffer means a dump of a partially-filled buffer will not print garbage if something goes wrong with the count tracking."
- "I am using `memcpy` for the event copy because it makes the cost explicit. In a hot path, if event_t were large, I might store pointers instead and manage a separate event pool."

---

## Solution 2: /proc File Parser

### Design Choices

The parser works on a copy of the input string because `strtok` is destructive. I use `strtol` instead of `atoi` because `strtol` provides error detection through the `endptr` parameter and `errno`. In kernel and systems tooling, silently misinterpreting a malformed value is worse than reporting a parse error.

The lookup function is a simple linear scan because `/proc/meminfo` has roughly 50 entries. A hash table would be overkill for this data size and would obscure the code.

```c
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

typedef struct {
    char key[64];
    uint64_t value_kb;
} meminfo_entry_t;

/*
 * parse_single_line: Parse one "Key:   12345 kB\n" line.
 * Returns 0 on success, -1 on malformed input.
 *
 * The approach: find the colon to split key from value, then use
 * strtoul on the numeric portion. We tolerate the "kB" suffix
 * being present or absent.
 */
static int parse_single_line(const char *line, meminfo_entry_t *entry)
{
    /* Find the colon separator. */
    const char *colon = strchr(line, ':');
    if (!colon)
        return -1;

    /* Extract key: everything before the colon. */
    size_t key_len = colon - line;
    if (key_len == 0 || key_len >= sizeof(entry->key))
        return -1;

    memcpy(entry->key, line, key_len);
    entry->key[key_len] = '\0';

    /* Trim trailing whitespace from key (rare but defensive). */
    while (key_len > 0 && entry->key[key_len - 1] == ' ')
        entry->key[--key_len] = '\0';

    /* Parse value: skip past the colon and any whitespace. */
    const char *val_start = colon + 1;
    while (*val_start == ' ' || *val_start == '\t')
        val_start++;

    if (*val_start == '\0' || *val_start == '\n')
        return -1;

    char *endptr;
    errno = 0;
    unsigned long val = strtoul(val_start, &endptr, 10);
    if (errno != 0 || endptr == val_start)
        return -1;

    entry->value_kb = (uint64_t)val;
    return 0;
}

int parse_meminfo(const char *input, meminfo_entry_t *entries, int max_entries)
{
    if (!input || !entries || max_entries <= 0)
        return -1;

    /*
     * Work on a copy because strtok modifies the string.
     * In a real tool, you might read line-by-line from a file descriptor
     * instead, avoiding the full copy.
     */
    char *copy = strdup(input);
    if (!copy)
        return -1;

    int count = 0;
    char *line = strtok(copy, "\n");

    while (line && count < max_entries) {
        /* Skip blank lines. */
        if (line[0] == '\0') {
            line = strtok(NULL, "\n");
            continue;
        }

        if (parse_single_line(line, &entries[count]) == 0)
            count++;
        /* Malformed lines are silently skipped. In production, I would
         * log a warning here. */

        line = strtok(NULL, "\n");
    }

    free(copy);
    return count;
}

int64_t lookup_meminfo(meminfo_entry_t *entries, int count, const char *key)
{
    if (!entries || !key)
        return -1;

    for (int i = 0; i < count; i++) {
        if (strcmp(entries[i].key, key) == 0)
            return (int64_t)entries[i].value_kb;
    }

    return -1;
}

void print_memory_summary(const char *raw_meminfo)
{
    meminfo_entry_t entries[64];
    int count = parse_meminfo(raw_meminfo, entries, 64);

    if (count < 0) {
        fprintf(stderr, "Failed to parse meminfo\n");
        return;
    }

    int64_t total = lookup_meminfo(entries, count, "MemTotal");
    int64_t free_mem = lookup_meminfo(entries, count, "MemFree");
    int64_t available = lookup_meminfo(entries, count, "MemAvailable");

    printf("Memory Summary:\n");

    if (total >= 0)
        printf("  Total:     %ld MB\n", (long)(total / 1024));
    else
        printf("  Total:     (not available)\n");

    if (free_mem >= 0)
        printf("  Free:      %ld MB\n", (long)(free_mem / 1024));
    else
        printf("  Free:      (not available)\n");

    if (available >= 0)
        printf("  Available: %ld MB\n", (long)(available / 1024));
    else
        printf("  Available: (not available)\n");

    if (total >= 0 && free_mem >= 0)
        printf("  Used:      %ld MB\n", (long)((total - free_mem) / 1024));
    else
        printf("  Used:      (cannot compute)\n");
}

int main(void)
{
    const char *test_input =
        "MemTotal:       32768000 kB\n"
        "MemFree:         1024000 kB\n"
        "MemAvailable:   16384000 kB\n"
        "Buffers:          512000 kB\n"
        "Cached:          8192000 kB\n"
        "SwapTotal:       4096000 kB\n"
        "SwapFree:        4096000 kB\n";

    /* Test the raw parser. */
    meminfo_entry_t entries[16];
    int count = parse_meminfo(test_input, entries, 16);
    printf("Parsed %d entries:\n", count);
    for (int i = 0; i < count; i++)
        printf("  %-20s %lu kB\n", entries[i].key, entries[i].value_kb);

    printf("\n");
    print_memory_summary(test_input);

    /* Edge case: empty input. */
    printf("\nEmpty input test:\n");
    count = parse_meminfo("", entries, 16);
    printf("  Parsed %d entries (expected 0)\n", count);

    /* Edge case: malformed line. */
    printf("\nMalformed input test:\n");
    count = parse_meminfo("NotAValidLine\nAlsoBad\nGood: 42 kB\n", entries, 16);
    printf("  Parsed %d entries (expected 1)\n", count);

    return 0;
}
```

### Interview Talking Points

- "I split the parser into a per-line helper because it makes the code testable in isolation. If a single line fails, the main loop can decide what to do (skip, warn, abort) without the per-line logic being aware of that policy."
- "I use `strtoul` with `endptr` checking rather than `atoi`. `atoi` returns 0 on failure, which is indistinguishable from the value actually being 0. `strtoul` lets me detect that the string was not a valid number."
- "At Oracle I wrote similar parsers for extracting hardware counter values from `perf stat` output and for reading `/proc/vmstat` in our memory soak tests."
- "In a real tool I would use `getline()` and read from a file descriptor instead of parsing a whole string in memory, especially for large /proc files."

---

## Solution 3: Bitmap / CPU Mask Operations

### Design Choices

This mirrors the kernel's `cpumask_t` and the underlying `bitmap.h` operations. The key insight is that we store the mask as an array of `unsigned long` words, and every operation decomposes the CPU number into a word index and a bit offset within that word.

I use compiler builtins (`__builtin_popcountl`, `__builtin_ctzl`) where they are available. These typically compile down to a single hardware instruction on x86 (`POPCNT`, `TZCNT`/`BSF`). In a real kernel build, you would use the kernel's own wrappers that handle fallback for older architectures.

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define MAX_CPUS 256
#define BITS_PER_LONG 64
#define LONGS_NEEDED ((MAX_CPUS + BITS_PER_LONG - 1) / BITS_PER_LONG)

typedef struct {
    unsigned long bits[LONGS_NEEDED];
} cpumask_t;

static inline int cpu_valid(int cpu)
{
    return cpu >= 0 && cpu < MAX_CPUS;
}

static inline int word_of(int cpu) { return cpu / BITS_PER_LONG; }
static inline int bit_of(int cpu)  { return cpu % BITS_PER_LONG; }

void cpumask_clear(cpumask_t *mask)
{
    memset(mask->bits, 0, sizeof(mask->bits));
}

int cpumask_set(cpumask_t *mask, int cpu)
{
    if (!mask || !cpu_valid(cpu))
        return -1;
    mask->bits[word_of(cpu)] |= (1UL << bit_of(cpu));
    return 0;
}

int cpumask_unset(cpumask_t *mask, int cpu)
{
    if (!mask || !cpu_valid(cpu))
        return -1;
    mask->bits[word_of(cpu)] &= ~(1UL << bit_of(cpu));
    return 0;
}

int cpumask_test(const cpumask_t *mask, int cpu)
{
    if (!mask || !cpu_valid(cpu))
        return -1;
    return (mask->bits[word_of(cpu)] >> bit_of(cpu)) & 1;
}

int cpumask_count(const cpumask_t *mask)
{
    if (!mask)
        return 0;

    int total = 0;
    for (int i = 0; i < LONGS_NEEDED; i++)
        total += __builtin_popcountl(mask->bits[i]);
    return total;
}

void cpumask_and(cpumask_t *dst, const cpumask_t *a, const cpumask_t *b)
{
    for (int i = 0; i < LONGS_NEEDED; i++)
        dst->bits[i] = a->bits[i] & b->bits[i];
}

void cpumask_or(cpumask_t *dst, const cpumask_t *a, const cpumask_t *b)
{
    for (int i = 0; i < LONGS_NEEDED; i++)
        dst->bits[i] = a->bits[i] | b->bits[i];
}

int cpumask_first_set(const cpumask_t *mask)
{
    if (!mask)
        return -1;

    for (int i = 0; i < LONGS_NEEDED; i++) {
        if (mask->bits[i] != 0) {
            /*
             * __builtin_ctzl returns the number of trailing zero bits,
             * which is the index of the lowest set bit within this word.
             */
            int bit = __builtin_ctzl(mask->bits[i]);
            int cpu = i * BITS_PER_LONG + bit;
            return (cpu < MAX_CPUS) ? cpu : -1;
        }
    }
    return -1;
}

int cpumask_next_set(const cpumask_t *mask, int prev)
{
    if (!mask)
        return -1;

    int start = prev + 1;
    if (start >= MAX_CPUS)
        return -1;

    /*
     * Start scanning from the word containing 'start'. Mask out the
     * bits below 'start' within that first word so we do not revisit
     * bits we have already passed.
     */
    int w = word_of(start);
    int b = bit_of(start);

    /* Mask off bits below 'start' in the first word. */
    unsigned long word = mask->bits[w] & (~0UL << b);

    if (word != 0) {
        int cpu = w * BITS_PER_LONG + __builtin_ctzl(word);
        return (cpu < MAX_CPUS) ? cpu : -1;
    }

    /* Scan subsequent words. */
    for (int i = w + 1; i < LONGS_NEEDED; i++) {
        if (mask->bits[i] != 0) {
            int cpu = i * BITS_PER_LONG + __builtin_ctzl(mask->bits[i]);
            return (cpu < MAX_CPUS) ? cpu : -1;
        }
    }

    return -1;
}

/*
 * Print in the range format used by /sys/devices/system/cpu/online.
 * Example: "0-3,8,12-15"
 */
void cpumask_print(const cpumask_t *mask)
{
    if (!mask) {
        printf("(null)\n");
        return;
    }

    int first = 1;  /* flag: is this the first range we print? */
    int cpu = cpumask_first_set(mask);

    while (cpu >= 0) {
        int range_start = cpu;

        /* Extend the range as long as consecutive bits are set. */
        while (1) {
            int next = cpumask_next_set(mask, cpu);
            if (next == cpu + 1)
                cpu = next;
            else
                break;
        }

        if (!first)
            printf(",");
        first = 0;

        if (cpu == range_start)
            printf("%d", range_start);
        else
            printf("%d-%d", range_start, cpu);

        cpu = cpumask_next_set(mask, cpu);
    }

    if (first)
        printf("(empty)");
    printf("\n");
}

int main(void)
{
    cpumask_t online, isolated, available;

    /* Simulate: CPUs 0-7 are online. */
    cpumask_clear(&online);
    for (int i = 0; i < 8; i++)
        cpumask_set(&online, i);

    /* CPUs 2-3 are isolated for a GPU workload. */
    cpumask_clear(&isolated);
    cpumask_set(&isolated, 2);
    cpumask_set(&isolated, 3);

    /* Available = online AND NOT isolated. */
    cpumask_t not_isolated;
    cpumask_clear(&not_isolated);
    for (int i = 0; i < MAX_CPUS; i++)
        cpumask_set(&not_isolated, i);

    /* XOR-style complement: invert isolated bits within the online set.
     * Simpler: just loop and test. */
    cpumask_clear(&available);
    for (int cpu = cpumask_first_set(&online); cpu >= 0;
         cpu = cpumask_next_set(&online, cpu)) {
        if (!cpumask_test(&isolated, cpu))
            cpumask_set(&available, cpu);
    }

    printf("Online:    "); cpumask_print(&online);
    printf("Isolated:  "); cpumask_print(&isolated);
    printf("Available: "); cpumask_print(&available);
    printf("Online count:    %d\n", cpumask_count(&online));
    printf("Available count: %d\n", cpumask_count(&available));

    /* Test AND/OR. */
    cpumask_t result;
    cpumask_and(&result, &online, &isolated);
    printf("AND(online, isolated): "); cpumask_print(&result);

    cpumask_or(&result, &available, &isolated);
    printf("OR(available, isolated): "); cpumask_print(&result);

    /* Edge case: empty mask. */
    cpumask_t empty;
    cpumask_clear(&empty);
    printf("Empty mask: "); cpumask_print(&empty);
    printf("First set in empty: %d (expected -1)\n", cpumask_first_set(&empty));

    /* Edge case: high-numbered CPU. */
    cpumask_t high;
    cpumask_clear(&high);
    cpumask_set(&high, 255);
    printf("High CPU mask: "); cpumask_print(&high);

    /* Out-of-range. */
    printf("Set cpu 256: %d (expected -1)\n", cpumask_set(&high, 256));
    printf("Set cpu -1:  %d (expected -1)\n", cpumask_set(&high, -1));

    return 0;
}
```

### Interview Talking Points

- "The word/bit decomposition is identical to how `include/linux/bitmap.h` works in the kernel. Every CPU mask operation ultimately becomes a shift and a bitwise OR/AND on a `unsigned long` array."
- "`__builtin_ctzl` compiles to `TZCNT` on x86 when available, which is a single-cycle instruction. This makes `first_set` and `next_set` very fast, which matters when iterating over large masks in the scheduler hot path."
- "The range printer is not just a nice-to-have. The kernel exposes this format in sysfs (`/sys/devices/system/cpu/online`), and I have parsed it many times when verifying CPU isolation in my benchmarking work."
- "I would add a `cpumask_andnot(dst, a, b)` for the 'available = online AND NOT isolated' pattern, which is cleaner than the manual loop I wrote in main. The kernel has this as a primitive."

---

## Solution 4: Reference-Counted Object

### Design Choices

The `kref_put` function takes a pointer-to-pointer so it can NULL out the caller's handle after freeing. This is a defensive pattern that prevents use-after-free. In the kernel, `kref_put` takes a release callback instead because the kernel cannot use pointer-to-pointer in all contexts (the `kref` is usually embedded in a larger struct). I use the pointer-to-pointer approach here because it is more appropriate for userspace C and clearly demonstrates the lifecycle.

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (*destroy_fn)(void *data);

typedef struct {
    int refcount;
    void *data;
    destroy_fn destroy;
} kref_obj_t;

kref_obj_t *kref_alloc(void *data, destroy_fn destroy)
{
    if (!data || !destroy)
        return NULL;

    kref_obj_t *obj = malloc(sizeof(kref_obj_t));
    if (!obj)
        return NULL;

    obj->refcount = 1;
    obj->data = data;
    obj->destroy = destroy;

    return obj;
}

kref_obj_t *kref_get(kref_obj_t *obj)
{
    if (!obj)
        return NULL;

    /*
     * In a threaded environment, this must be atomic:
     *   atomic_fetch_add(&obj->refcount, 1);
     * For this single-threaded exercise, a plain increment is correct.
     */
    obj->refcount++;
    return obj;
}

void kref_put(kref_obj_t **obj_ptr)
{
    if (!obj_ptr || !*obj_ptr)
        return;

    kref_obj_t *obj = *obj_ptr;

    obj->refcount--;

    /*
     * If refcount hits zero, we own the last reference. Call the
     * destroy callback on the data, then free the kref wrapper.
     *
     * In the kernel, a refcount reaching zero is a one-way transition.
     * If it ever goes below zero, that indicates a double-put bug.
     * A production implementation would BUG_ON(refcount < 0) here.
     */
    if (obj->refcount == 0) {
        if (obj->destroy && obj->data)
            obj->destroy(obj->data);
        free(obj);
    } else if (obj->refcount < 0) {
        fprintf(stderr, "BUG: refcount went negative on object %p\n",
                (void *)obj);
        /* In kernel: BUG(); */
    }

    /*
     * NULL out the caller's pointer regardless of whether we freed.
     * This ensures the caller cannot use their stale handle.
     *
     * A more conservative approach is to only NULL on free. I NULL
     * unconditionally because after a put, the caller has logically
     * relinquished their reference and should not access the object
     * through this pointer anymore.
     */
    *obj_ptr = NULL;
}

int kref_count(const kref_obj_t *obj)
{
    if (!obj)
        return -1;
    return obj->refcount;
}

/* ---- Example usage ---- */

typedef struct {
    int fd;
    char name[64];
    size_t buffer_size;
    char *buffer;
} device_ctx_t;

static int destroy_called = 0;

void device_ctx_destroy(void *data)
{
    device_ctx_t *ctx = (device_ctx_t *)data;
    if (ctx) {
        printf("  Destroying device: %s (fd=%d)\n", ctx->name, ctx->fd);
        free(ctx->buffer);
        free(ctx);
        destroy_called++;
    }
}

int main(void)
{
    /* Step 1: Allocate the data on the heap. */
    device_ctx_t *dev = malloc(sizeof(device_ctx_t));
    if (!dev) {
        fprintf(stderr, "malloc failed\n");
        return 1;
    }
    dev->fd = 42;
    strncpy(dev->name, "gpu0", sizeof(dev->name) - 1);
    dev->name[sizeof(dev->name) - 1] = '\0';
    dev->buffer_size = 4096;
    dev->buffer = malloc(dev->buffer_size);
    if (!dev->buffer) {
        free(dev);
        fprintf(stderr, "buffer malloc failed\n");
        return 1;
    }
    memset(dev->buffer, 0, dev->buffer_size);

    /* Step 2: Wrap it in a ref-counted object. Refcount starts at 1. */
    kref_obj_t *ref1 = kref_alloc(dev, device_ctx_destroy);
    printf("After alloc:   refcount = %d\n", kref_count(ref1));

    /* Step 3: Take two additional references. */
    kref_obj_t *ref2 = kref_get(ref1);
    kref_obj_t *ref3 = kref_get(ref1);
    printf("After 2 gets:  refcount = %d\n", kref_count(ref1));

    /* Step 4: Drop all three references. */
    printf("Dropping ref3...\n");
    kref_put(&ref3);
    printf("  ref3 is now %s, refcount via ref1 = %d\n",
           ref3 ? "non-NULL (BUG)" : "NULL", kref_count(ref1));

    printf("Dropping ref2...\n");
    kref_put(&ref2);
    printf("  ref2 is now %s, refcount via ref1 = %d\n",
           ref2 ? "non-NULL (BUG)" : "NULL", kref_count(ref1));

    printf("Dropping ref1 (last reference)...\n");
    kref_put(&ref1);
    printf("  ref1 is now %s\n", ref1 ? "non-NULL (BUG)" : "NULL");

    /* Step 5: Verify destroy was called exactly once. */
    printf("\nDestroy callback invoked %d time(s) (expected 1).\n",
           destroy_called);

    return 0;
}
```

### Interview Talking Points

- "In the kernel, `kref` is embedded inside the object struct itself using `struct kref`, not wrapped around it. The `container_of` macro is used to get from the `kref` pointer back to the enclosing struct. I used a wrapper pattern here for clarity, but I am familiar with the embedded approach."
- "The kernel's `kref_put` uses `refcount_dec_and_test`, which is an atomic decrement that returns true if the count reached zero. This is safe for concurrent access without an external lock, because the atomic compare-and-swap ensures exactly one thread sees the zero transition."
- "I added the `refcount < 0` check as a debug assertion. In the kernel, `refcount_t` (the hardened version of `atomic_t`) traps on underflow and saturation, which catches double-free bugs that plain `atomic_t` would silently allow."

---

## Solution 5: Find the Bug (Annotated)

Here is the corrected version with each fix annotated.

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char hostname[128];
    int cpu_count;
    double load_avg;
    int is_healthy;
} node_info_t;

typedef struct {
    node_info_t *nodes;
    int count;
    int capacity;
} node_list_t;

node_list_t *node_list_create(int initial_capacity)
{
    /*
     * FIX 1: Validate initial_capacity.
     * Original had no check. If capacity is 0, malloc(0) is
     * implementation-defined (may return NULL or a unique pointer).
     * Either way, capacity *= 2 in add() would stay 0 forever.
     */
    if (initial_capacity <= 0)
        return NULL;

    node_list_t *list = malloc(sizeof(node_list_t));
    /* FIX 2: NULL check on malloc. */
    if (!list)
        return NULL;

    list->nodes = malloc(initial_capacity * sizeof(node_info_t));
    /* FIX 3: NULL check on second malloc; clean up first on failure. */
    if (!list->nodes) {
        free(list);
        return NULL;
    }

    list->count = 0;
    list->capacity = initial_capacity;
    return list;
}

int node_list_add(node_list_t *list, const char *hostname,
                  int cpu_count, double load_avg)
{
    if (!list || !hostname)
        return -1;

    if (list->count >= list->capacity) {
        int new_capacity = list->capacity * 2;

        /*
         * FIX 4: Use a temporary for realloc.
         * Original: list->nodes = realloc(list->nodes, ...)
         * If realloc fails and returns NULL, the original pointer
         * is lost. This is both a memory leak and a guaranteed
         * crash on the next access.
         */
        node_info_t *tmp = realloc(list->nodes,
                                   new_capacity * sizeof(node_info_t));
        if (!tmp)
            return -1;

        list->nodes = tmp;
        list->capacity = new_capacity;
    }

    node_info_t *node = &list->nodes[list->count];

    /*
     * FIX 5: Use strncpy with explicit null termination.
     * Original: strcpy(node->hostname, hostname)
     * If hostname is longer than 127 chars, this is a stack/heap
     * buffer overflow. In a security context, this is exploitable.
     */
    strncpy(node->hostname, hostname, sizeof(node->hostname) - 1);
    node->hostname[sizeof(node->hostname) - 1] = '\0';

    node->cpu_count = cpu_count;
    node->load_avg = load_avg;

    /*
     * FIX 6: Use a more meaningful health check.
     * Original: load_avg < cpu_count
     * This is technically valid C (implicit int-to-double promotion),
     * but the semantic is questionable. A load average of 3.9 on a
     * 4-core machine would be "healthy," which is debatable.
     * A per-core threshold is more standard.
     */
    node->is_healthy = (cpu_count > 0) &&
                       (load_avg / (double)cpu_count < 0.8);

    list->count++;
    return list->count;
}

node_info_t *node_list_find(node_list_t *list, const char *hostname)
{
    if (!list || !hostname)
        return NULL;

    /*
     * FIX 7: Change <= to <.
     * Original: i <= list->count
     * When i == list->count, we read list->nodes[count], which is
     * one past the valid range. This is an out-of-bounds read.
     */
    for (int i = 0; i < list->count; i++) {
        if (strcmp(list->nodes[i].hostname, hostname) == 0)
            return &list->nodes[i];
    }
    return NULL;
}

void node_list_remove(node_list_t *list, int index)
{
    if (!list)
        return;

    /*
     * FIX 8: Change > to >=.
     * Original: index > list->count
     * An index equal to count is out of bounds (valid indices are
     * 0 through count-1). The original allowed index == count
     * to proceed, leading to the shift loop reading past valid data.
     */
    if (index < 0 || index >= list->count)
        return;

    /*
     * FIX 9: Change loop bound from list->count to list->count - 1.
     * Original: i < list->count
     * When i == list->count - 1, the copy reads nodes[list->count],
     * which is uninitialized or out of bounds. The correct bound is
     * count - 1 because the last valid source index is count - 1,
     * and the last shift copies nodes[count-1] into nodes[count-2].
     */
    for (int i = index; i < list->count - 1; i++)
        list->nodes[i] = list->nodes[i + 1];

    list->count--;
}

void node_list_destroy(node_list_t *list)
{
    /* FIX 10: NULL check. */
    if (!list)
        return;
    free(list->nodes);
    free(list);
}

int main(void)
{
    node_list_t *list = node_list_create(2);
    if (!list) {
        fprintf(stderr, "Failed to create list\n");
        return 1;
    }

    node_list_add(list, "gpu-node-001", 64, 12.5);
    node_list_add(list, "gpu-node-002", 128, 95.0);
    node_list_add(list, "gpu-node-003", 64, 60.0);  /* triggers realloc */

    printf("Nodes in list: %d\n", list->count);

    node_info_t *found = node_list_find(list, "gpu-node-002");
    if (found)
        printf("Found: %s, healthy=%d\n", found->hostname, found->is_healthy);

    node_list_remove(list, 1);
    printf("After remove: %d nodes\n", list->count);

    /* Verify gpu-node-003 shifted into index 1. */
    found = node_list_find(list, "gpu-node-003");
    if (found)
        printf("Found after shift: %s\n", found->hostname);

    node_list_destroy(list);
    printf("Destroyed. Done.\n");

    /* Edge cases. */
    node_list_destroy(NULL);  /* should not crash */

    return 0;
}
```

### Interview Talking Points

- "The realloc pattern is probably the most commonly missed bug in C codebases. I have seen it in production code. The fix is simple: assign to a temp, check for NULL, and only then update the real pointer."
- "The off-by-one in `node_list_find` and `node_list_remove` is a classic fencepost error. I always ask myself: is the bound inclusive or exclusive? For a count-based array, the last valid index is `count - 1`, so the loop condition must be `<`, never `<=`."
- "The `strcpy` overflow is a CWE-120 (buffer copy without size check). In kernel code, we use `strscpy` (which returns an error on truncation) rather than `strncpy` (which pads with zeros and does not guarantee termination if the source fills the buffer)."

---

## Solution 6: Simple Hash Table

### Design Choices

I use the FNV-1a hash function because it has excellent distribution properties for short strings and is trivial to implement. djb2 is equally common and also acceptable. The key design principle in the chaining approach is that each entry owns a `strdup`'d copy of the key. This means the caller is free to modify or free their key string after insertion without corrupting the table.

```c
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HT_DEFAULT_SIZE 64

typedef struct ht_entry {
    char *key;
    int value;
    struct ht_entry *next;
} ht_entry_t;

typedef struct {
    ht_entry_t **buckets;
    int size;
    int count;
} hashtable_t;

/*
 * FNV-1a hash for strings.
 * Chosen for simplicity and good distribution on short keys.
 * The magic constants are from the FNV specification.
 */
unsigned int ht_hash(const char *key, int size)
{
    unsigned int hash = 2166136261u;

    for (const char *p = key; *p; p++) {
        hash ^= (unsigned char)*p;
        hash *= 16777619u;
    }

    return hash % (unsigned int)size;
}

hashtable_t *ht_create(int size)
{
    if (size <= 0)
        size = HT_DEFAULT_SIZE;

    hashtable_t *ht = malloc(sizeof(hashtable_t));
    if (!ht)
        return NULL;

    ht->buckets = calloc(size, sizeof(ht_entry_t *));
    if (!ht->buckets) {
        free(ht);
        return NULL;
    }

    ht->size = size;
    ht->count = 0;
    return ht;
}

int ht_insert(hashtable_t *ht, const char *key, int value)
{
    if (!ht || !key)
        return -1;

    unsigned int idx = ht_hash(key, ht->size);

    /* Check if the key already exists; if so, update in place. */
    ht_entry_t *curr = ht->buckets[idx];
    while (curr) {
        if (strcmp(curr->key, key) == 0) {
            curr->value = value;
            return 0;  /* updated, count unchanged */
        }
        curr = curr->next;
    }

    /* Key does not exist. Create a new entry at the head of the chain. */
    ht_entry_t *entry = malloc(sizeof(ht_entry_t));
    if (!entry)
        return -1;

    entry->key = strdup(key);
    if (!entry->key) {
        free(entry);
        return -1;
    }

    entry->value = value;
    entry->next = ht->buckets[idx];
    ht->buckets[idx] = entry;
    ht->count++;

    return 0;
}

int ht_lookup(hashtable_t *ht, const char *key, int *value_out)
{
    if (!ht || !key)
        return -1;

    unsigned int idx = ht_hash(key, ht->size);
    ht_entry_t *curr = ht->buckets[idx];

    while (curr) {
        if (strcmp(curr->key, key) == 0) {
            if (value_out)
                *value_out = curr->value;
            return 0;
        }
        curr = curr->next;
    }

    return -1;
}

int ht_delete(hashtable_t *ht, const char *key)
{
    if (!ht || !key)
        return -1;

    unsigned int idx = ht_hash(key, ht->size);

    /*
     * Use a pointer-to-pointer to simplify deletion.
     * This avoids a special case for deleting the head of the chain.
     * pp points to the "next" field (or bucket slot) that leads to
     * the current entry. To unlink, we set *pp = curr->next.
     */
    ht_entry_t **pp = &ht->buckets[idx];
    while (*pp) {
        ht_entry_t *curr = *pp;
        if (strcmp(curr->key, key) == 0) {
            *pp = curr->next;
            free(curr->key);
            free(curr);
            ht->count--;
            return 0;
        }
        pp = &curr->next;
    }

    return -1;
}

void ht_destroy(hashtable_t *ht)
{
    if (!ht)
        return;

    for (int i = 0; i < ht->size; i++) {
        ht_entry_t *curr = ht->buckets[i];
        while (curr) {
            ht_entry_t *next = curr->next;
            free(curr->key);
            free(curr);
            curr = next;
        }
    }

    free(ht->buckets);
    free(ht);
}

void ht_print_stats(hashtable_t *ht)
{
    if (!ht) {
        printf("(null table)\n");
        return;
    }

    int longest_chain = 0;
    int empty_buckets = 0;

    for (int i = 0; i < ht->size; i++) {
        int chain_len = 0;
        ht_entry_t *curr = ht->buckets[i];
        while (curr) {
            chain_len++;
            curr = curr->next;
        }
        if (chain_len == 0)
            empty_buckets++;
        if (chain_len > longest_chain)
            longest_chain = chain_len;
    }

    printf("Hash table stats:\n");
    printf("  Entries:        %d\n", ht->count);
    printf("  Buckets:        %d\n", ht->size);
    printf("  Load factor:    %.2f\n", (double)ht->count / ht->size);
    printf("  Longest chain:  %d\n", longest_chain);
    printf("  Empty buckets:  %d (%.1f%%)\n",
           empty_buckets, 100.0 * empty_buckets / ht->size);
}

int main(void)
{
    hashtable_t *ht = ht_create(8);  /* small for testing collisions */

    /* Insert kernel module names and load states. */
    ht_insert(ht, "nvidia", 1);
    ht_insert(ht, "nccl", 1);
    ht_insert(ht, "ib_core", 1);
    ht_insert(ht, "mlx5_core", 1);
    ht_insert(ht, "nvme", 1);
    ht_insert(ht, "vfio", 0);
    ht_insert(ht, "kvm", 0);

    ht_print_stats(ht);

    /* Lookup. */
    int val;
    if (ht_lookup(ht, "nvidia", &val) == 0)
        printf("\nnvidia loaded: %d\n", val);

    if (ht_lookup(ht, "nonexistent", &val) != 0)
        printf("nonexistent: not found (correct)\n");

    /* Update. */
    ht_insert(ht, "vfio", 1);
    ht_lookup(ht, "vfio", &val);
    printf("vfio after update: %d\n", val);

    /* Delete. */
    ht_delete(ht, "kvm");
    printf("kvm after delete: %s\n",
           ht_lookup(ht, "kvm", &val) == 0 ? "found (BUG)" : "not found");

    printf("\nAfter operations:\n");
    ht_print_stats(ht);

    ht_destroy(ht);
    printf("\nDestroyed. Done.\n");
    return 0;
}
```

### Interview Talking Points

- "The pointer-to-pointer deletion technique eliminates the special case for head-of-list removal. It is the same pattern used throughout the Linux kernel's linked list operations, where `list_del` works uniformly regardless of position."
- "I chose FNV-1a over djb2 because the XOR-then-multiply order in FNV-1a produces better avalanching for keys that share common prefixes, which is common with kernel module names like `mlx5_core` and `mlx5_ib`."
- "In a production context, I would add resize/rehash logic when the load factor exceeds 0.75. The typical approach is to double the bucket count and reinsert all entries, amortizing the cost over many insertions."
- "The `strdup` on keys is critical. Without it, the table's correctness depends on the caller keeping their string alive, which is fragile. Ownership semantics should be explicit."

---

## Solution 7: Simple Memory Pool Allocator

### Design Choices

The embedded free list is the core trick. When a block is free, its first `sizeof(void*)` bytes store a pointer to the next free block. When the block is allocated and handed to the user, those bytes are available for the user's data. This means allocation and deallocation are both O(1), which is the entire point of a pool allocator.

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef struct {
    void *memory;       /* base of the raw allocation */
    void *free_list;    /* head of the embedded free list */
    size_t block_size;  /* effective block size (may be rounded up) */
    int total_blocks;
    int free_count;
} mempool_t;

mempool_t *mempool_create(size_t block_size, int num_blocks)
{
    if (num_blocks <= 0)
        return NULL;

    /*
     * Each free block must hold at least a pointer for the free list.
     * If the requested block size is smaller, round up.
     */
    if (block_size < sizeof(void *))
        block_size = sizeof(void *);

    /*
     * Optionally align block_size to sizeof(void*) for pointer alignment.
     * This ensures that the embedded free-list pointer is always
     * naturally aligned, avoiding undefined behavior on architectures
     * that require aligned pointer access.
     */
    size_t alignment = sizeof(void *);
    block_size = (block_size + alignment - 1) & ~(alignment - 1);

    mempool_t *pool = malloc(sizeof(mempool_t));
    if (!pool)
        return NULL;

    pool->memory = malloc(block_size * num_blocks);
    if (!pool->memory) {
        free(pool);
        return NULL;
    }

    pool->block_size = block_size;
    pool->total_blocks = num_blocks;
    pool->free_count = num_blocks;

    /*
     * Initialize the free list by threading all blocks together.
     * Each block's first sizeof(void*) bytes point to the next block.
     * The last block points to NULL.
     *
     * Memory layout:
     *   [block 0] -> [block 1] -> [block 2] -> ... -> [block N-1] -> NULL
     */
    pool->free_list = pool->memory;

    char *ptr = (char *)pool->memory;
    for (int i = 0; i < num_blocks - 1; i++) {
        void *next = ptr + block_size;
        *(void **)ptr = next;      /* embed the next-pointer */
        ptr += block_size;
    }
    *(void **)ptr = NULL;          /* last block has no next */

    return pool;
}

void *mempool_alloc(mempool_t *pool)
{
    if (!pool || !pool->free_list)
        return NULL;

    /*
     * Pop the head of the free list. O(1).
     * Read the next-pointer stored in this block before returning it.
     */
    void *block = pool->free_list;
    pool->free_list = *(void **)block;
    pool->free_count--;

    /*
     * Zero the block before handing it to the user. This prevents
     * information leaks from previously freed blocks and ensures
     * the old free-list pointer is not visible to the user.
     */
    memset(block, 0, pool->block_size);

    return block;
}

void mempool_free(mempool_t *pool, void *ptr)
{
    if (!pool || !ptr)
        return;

    /*
     * Push this block onto the head of the free list. O(1).
     * Write the current free_list head into the first bytes of the
     * block, then point free_list at this block.
     *
     * NOTE: A production implementation would validate that ptr falls
     * within [pool->memory, pool->memory + total_blocks * block_size)
     * and is aligned to block_size. Without this check, freeing a
     * pointer that does not belong to this pool corrupts the free list.
     */
    *(void **)ptr = pool->free_list;
    pool->free_list = ptr;
    pool->free_count++;
}

void mempool_stats(mempool_t *pool)
{
    if (!pool) {
        printf("(null pool)\n");
        return;
    }
    printf("Memory pool stats:\n");
    printf("  Block size:  %zu bytes\n", pool->block_size);
    printf("  Total:       %d blocks\n", pool->total_blocks);
    printf("  Free:        %d blocks\n", pool->free_count);
    printf("  Allocated:   %d blocks\n", pool->total_blocks - pool->free_count);
}

void mempool_destroy(mempool_t *pool)
{
    if (!pool)
        return;
    free(pool->memory);
    free(pool);
}

/* ---- Test harness ---- */

typedef struct {
    uint64_t timestamp;
    uint32_t event_id;
    uint32_t flags;
} event_record_t;

int main(void)
{
    /* Create a pool of 5 event records. */
    mempool_t *pool = mempool_create(sizeof(event_record_t), 5);
    if (!pool) {
        fprintf(stderr, "Pool creation failed\n");
        return 1;
    }

    printf("After creation:\n");
    mempool_stats(pool);

    /* Allocate 3 records. */
    event_record_t *e1 = mempool_alloc(pool);
    event_record_t *e2 = mempool_alloc(pool);
    event_record_t *e3 = mempool_alloc(pool);

    e1->timestamp = 1000;
    e1->event_id = 1;
    e2->timestamp = 2000;
    e2->event_id = 2;
    e3->timestamp = 3000;
    e3->event_id = 3;

    printf("\nAfter 3 allocs:\n");
    mempool_stats(pool);

    /* Free the middle one and reallocate. */
    mempool_free(pool, e2);
    printf("\nAfter freeing e2:\n");
    mempool_stats(pool);

    event_record_t *e4 = mempool_alloc(pool);
    printf("e4 pointer == old e2 pointer? %s\n",
           (void *)e4 == (void *)e2 ? "yes (reused)" : "no");
    printf("e4->event_id after alloc: %u (should be 0, memset'd)\n",
           e4->event_id);

    /* Exhaust the pool. */
    (void)mempool_alloc(pool);  /* e5 */
    (void)mempool_alloc(pool);  /* e6 */
    event_record_t *e7 = mempool_alloc(pool);  /* should be NULL */

    printf("\nAfter exhausting pool:\n");
    mempool_stats(pool);
    printf("e7 (over-alloc): %s\n", e7 ? "non-NULL (BUG)" : "NULL (correct)");

    mempool_destroy(pool);
    printf("\nDestroyed. Done.\n");
    return 0;
}
```

### Interview Talking Points

- "This is the core idea behind the kernel's slab allocator (`kmem_cache_create` / `kmem_cache_alloc`). The kernel's SLUB allocator uses the same embedded free-list technique, storing the next-pointer inside free objects. SLUB also adds per-CPU freelists to avoid lock contention."
- "I align the block size to `sizeof(void*)` to guarantee that the embedded pointer is naturally aligned. On some ARM variants, an unaligned pointer dereference is either a trap or silently corrupted data."
- "The `memset` on allocation is a defense-in-depth measure. It prevents the free-list pointer from leaking into user data, which could be a security concern in a multi-tenant environment like CoreWeave's cloud."
- "A real pool allocator would also support growing (allocating a new slab of blocks when exhausted) and shrinking (returning empty slabs to the page allocator). The kernel does this through the slab reaper."

---

## Solution 8: Process Exec Argument Builder

### Design Choices

The function works on a copy of the input string because tokenization is destructive. I grow the argv array with `realloc` using a doubling strategy, which amortizes the cost to O(1) per insertion. The critical detail is the NULL terminator at the end of argv, which `execv` and `execvp` require to know where the argument list ends.

```c
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/*
 * build_argv: Split a command line string into an execv-style argv array.
 *
 * Handles:
 *   - Leading, trailing, and multiple consecutive whitespace
 *   - Single-quoted strings (no escape sequences)
 *   - Returns a NULL-terminated argv array
 *
 * The caller is responsible for calling free_argv when done.
 */
char **build_argv(const char *cmdline, int *argc_out)
{
    if (!cmdline || !argc_out)
        return NULL;

    *argc_out = 0;

    /* Work on a copy. */
    char *copy = strdup(cmdline);
    if (!copy)
        return NULL;

    int capacity = 8;
    char **argv = malloc(capacity * sizeof(char *));
    if (!argv) {
        free(copy);
        return NULL;
    }

    int argc = 0;
    const char *p = copy;

    while (*p) {
        /* Skip whitespace between arguments. */
        while (*p && isspace((unsigned char)*p))
            p++;

        if (*p == '\0')
            break;

        const char *start;
        const char *end;

        if (*p == '\'') {
            /*
             * Quoted argument: find the closing quote.
             * The quotes themselves are not included in the argument.
             */
            p++;  /* skip opening quote */
            start = p;
            while (*p && *p != '\'')
                p++;
            end = p;
            if (*p == '\'')
                p++;  /* skip closing quote */
        } else {
            /* Unquoted argument: runs until whitespace or end. */
            start = p;
            while (*p && !isspace((unsigned char)*p))
                p++;
            end = p;
        }

        /* Extract the argument. */
        size_t len = end - start;
        if (len == 0)
            continue;

        /* Grow argv if needed. Reserve space for the NULL terminator. */
        if (argc + 1 >= capacity) {
            capacity *= 2;
            char **tmp = realloc(argv, capacity * sizeof(char *));
            if (!tmp) {
                /* Cleanup on failure: free everything allocated so far. */
                for (int i = 0; i < argc; i++)
                    free(argv[i]);
                free(argv);
                free(copy);
                return NULL;
            }
            argv = tmp;
        }

        /*
         * Allocate and copy the argument. We cannot use strdup here
         * because the token is not null-terminated in our copy buffer
         * (it is delimited by whitespace or quote, not '\0').
         */
        argv[argc] = malloc(len + 1);
        if (!argv[argc]) {
            for (int i = 0; i < argc; i++)
                free(argv[i]);
            free(argv);
            free(copy);
            return NULL;
        }

        memcpy(argv[argc], start, len);
        argv[argc][len] = '\0';
        argc++;
    }

    /* NULL-terminate the argv array (required by execv). */
    argv[argc] = NULL;

    free(copy);
    *argc_out = argc;
    return argv;
}

void free_argv(char **argv, int argc)
{
    if (!argv)
        return;
    for (int i = 0; i < argc; i++)
        free(argv[i]);
    free(argv);
}

int main(void)
{
    /* Test 1: Basic splitting with extra whitespace. */
    printf("Test 1: Basic splitting\n");
    int argc;
    char **argv = build_argv("  /usr/bin/perf  stat  -e cycles  ./workload  ", &argc);
    if (argv) {
        printf("  argc = %d\n", argc);
        for (int i = 0; i < argc; i++)
            printf("  argv[%d] = \"%s\"\n", i, argv[i]);
        printf("  argv[%d] = %s\n", argc, argv[argc] ? "non-NULL (BUG)" : "NULL");
        free_argv(argv, argc);
    }

    /* Test 2: Single-quoted argument. */
    printf("\nTest 2: Quoted argument\n");
    argv = build_argv("grep 'hello world' file.txt", &argc);
    if (argv) {
        printf("  argc = %d\n", argc);
        for (int i = 0; i < argc; i++)
            printf("  argv[%d] = \"%s\"\n", i, argv[i]);
        free_argv(argv, argc);
    }

    /* Test 3: Empty input. */
    printf("\nTest 3: Empty input\n");
    argv = build_argv("", &argc);
    if (argv) {
        printf("  argc = %d (expected 0)\n", argc);
        printf("  argv[0] = %s\n", argv[0] ? "non-NULL (BUG)" : "NULL");
        free_argv(argv, argc);
    }

    /* Test 4: Only whitespace. */
    printf("\nTest 4: Only whitespace\n");
    argv = build_argv("   \t  \n  ", &argc);
    if (argv) {
        printf("  argc = %d (expected 0)\n", argc);
        free_argv(argv, argc);
    }

    /* Test 5: Single argument, no whitespace. */
    printf("\nTest 5: Single argument\n");
    argv = build_argv("ls", &argc);
    if (argv) {
        printf("  argc = %d, argv[0] = \"%s\"\n", argc, argv[0]);
        free_argv(argv, argc);
    }

    /* Test 6: Adjacent quotes. */
    printf("\nTest 6: Adjacent quotes\n");
    argv = build_argv("echo 'foo bar' 'baz qux'", &argc);
    if (argv) {
        printf("  argc = %d\n", argc);
        for (int i = 0; i < argc; i++)
            printf("  argv[%d] = \"%s\"\n", i, argv[i]);
        free_argv(argv, argc);
    }

    printf("\nAll tests complete.\n");
    return 0;
}
```

### Interview Talking Points

- "The cleanup-on-failure logic in `build_argv` is important. If any allocation fails mid-parse, I free everything already allocated before returning NULL. This is a common pattern in kernel code where functions that allocate multiple resources must have a cleanup path that reverses each allocation in order."
- "I cast the argument to `isspace` through `unsigned char` because `isspace` takes an `int` and has undefined behavior if the value is negative (which happens with `char` on platforms where `char` is signed and the byte is > 127). This is a subtle but real portability issue."
- "The `execv` contract requires argv to be NULL-terminated. Forgetting this is a surprisingly common bug. The kernel's `do_execveat_common` reads argv entries until it hits NULL, so a missing terminator would read garbage memory."
- "I chose manual pointer-walking over `strtok` because `strtok` cannot handle the quoted-string case. `strtok` only splits on single characters and cannot distinguish between a space inside quotes and a space as a delimiter."

---

## Summary: What to Remember on Interview Day

1. **Always check your allocations.** Every `malloc`, `calloc`, `realloc`, and `strdup` can fail. Show this awareness in every function.

2. **Own your memory.** For every allocation, know exactly where the corresponding `free` lives. If an object is shared, say "this needs reference counting or clear ownership rules."

3. **Use `realloc` safely.** Always assign to a temporary. This is a pattern they will notice.

4. **Prefer `strncpy`+manual null-termination over `strcpy`.** Better yet, mention `strscpy` from the kernel as the ideal choice.

5. **Use `strtol`/`strtoul` over `atoi`.** The error detection through `endptr` is what separates production code from throwaway scripts.

6. **Bounds check everything.** Array indices, CPU numbers, buffer sizes. Say "this is a bounds check" out loud so the interviewer knows it is intentional.

7. **Connect to the kernel.** When you write a ring buffer, say "ftrace." When you write a memory pool, say "SLUB." When you write a bitmask, say "cpumask_t." This signals domain fluency without being forced.

8. **Talk about what you would do differently in production.** Thread safety (atomics, per-CPU data), alignment, error logging, and testing are all good things to mention after you finish the core implementation.

9. **Compile early, compile often.** In CoderPad, compile after writing each function. Do not write 100 lines blind.

10. **Keep it clean.** Consistent naming, braces on their own lines (kernel style) or consistently K&R, and functions under 40 lines. Readability is a signal.
