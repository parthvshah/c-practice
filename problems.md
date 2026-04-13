# CoreWeave Systems Kernel Engineer: C Practice Problems

**Target Role:** Systems Engineer, Kernel (HAVOCK Team)
**Format:** CoderPad, live coding in C
**Interview Style:** Day-to-day problems, not leetcode

---

## How to Use This Set

- Open [CoderPad Sandbox](https://app.coderpad.io/sandbox) or a local editor with `gcc`.
- Set a timer for each problem (noted in the header).
- Write compilable, clean C. No external libraries beyond libc.
- Talk out loud as you code, as you would in the real interview.
- After each problem, review: does it compile cleanly with `-Wall -Werror`? Did you handle edge cases?

---

## Problem 1: Ring Buffer for Kernel Event Log

**Time: 25 minutes** | **Category: Data Structures, Systems Patterns**

### Scenario

You are building a diagnostic event logger for a bare-metal fleet. Events arrive faster than they can be consumed, so you need a fixed-size ring buffer that overwrites the oldest entries when full. This is a common pattern in kernel trace buffers (`ftrace`, `perf_event`).

### Requirements

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

/*
 * Implement these functions:
 *
 * ring_buf_t *ring_buf_create(int capacity);
 *     Allocate and return a new ring buffer. Return NULL on failure.
 *
 * int ring_buf_push(ring_buf_t *rb, const event_t *event);
 *     Insert an event. Overwrite oldest if full. Return 0 on success, -1 on error.
 *
 * int ring_buf_pop(ring_buf_t *rb, event_t *out);
 *     Remove and copy the oldest event into *out. Return 0 on success, -1 if empty.
 *
 * int ring_buf_dump(ring_buf_t *rb, int min_severity);
 *     Print all events with severity >= min_severity, oldest first.
 *     Return the number of events printed.
 *
 * void ring_buf_destroy(ring_buf_t *rb);
 *     Free all resources.
 */
```

### What They Are Looking For

- Correct modular arithmetic for head/tail indexing.
- Overwrite semantics when full (head advances, count stays at capacity).
- Clean memory lifecycle: allocation failure checks, no leaks.
- Edge cases: empty buffer pop, zero capacity, NULL pointers.

### Talking Points While Coding

- "This mirrors how ftrace's ring buffer works, where per-CPU buffers overwrite old data."
- "In production I would make this lock-free with atomic head/tail for multi-producer use."

---

## Problem 2: /proc File Parser

**Time: 20 minutes** | **Category: String/Buffer Parsing**

### Scenario

You need a diagnostic tool that reads `/proc/meminfo`-style data and extracts specific fields. The input is a multiline string with `key: value kB` format. This is bread-and-butter work for kernel observability tooling.

### Requirements

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef struct {
    char key[64];
    uint64_t value_kb;
} meminfo_entry_t;

/*
 * Given a string in /proc/meminfo format:
 *
 *   "MemTotal:       32768000 kB\n"
 *   "MemFree:         1024000 kB\n"
 *   "MemAvailable:   16384000 kB\n"
 *   "Buffers:          512000 kB\n"
 *   ...
 *
 * Implement:
 *
 * int parse_meminfo(const char *input, meminfo_entry_t *entries, int max_entries);
 *     Parse the input string into the entries array.
 *     Return the number of entries parsed, or -1 on error.
 *
 * int64_t lookup_meminfo(meminfo_entry_t *entries, int count, const char *key);
 *     Return the value_kb for a given key, or -1 if not found.
 *
 * void print_memory_summary(const char *raw_meminfo);
 *     Parse and print a summary showing:
 *       Total, Free, Available, and Used (Total - Free) in MB.
 *     Handle missing fields gracefully.
 */
```

### Test Input

```c
const char *test_input =
    "MemTotal:       32768000 kB\n"
    "MemFree:         1024000 kB\n"
    "MemAvailable:   16384000 kB\n"
    "Buffers:          512000 kB\n"
    "Cached:          8192000 kB\n"
    "SwapTotal:       4096000 kB\n"
    "SwapFree:        4096000 kB\n";
```

### What They Are Looking For

- Robust parsing that handles variable whitespace.
- Correct use of `strtol`/`strtoul` with error checking (not just `atoi`).
- Defensive coding: what if a line is malformed? What if "kB" is missing?
- Clean separation between parsing and lookup logic.

---

## Problem 3: Bitmap / CPU Mask Operations

**Time: 20 minutes** | **Category: Bit Manipulation**

### Scenario

CoreWeave manages large bare-metal nodes with many CPU cores. You need to implement a CPU mask type that tracks which cores are online, isolated, or assigned to workloads. This is directly analogous to the kernel's `cpumask_t`.

### Requirements

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

/*
 * Implement:
 *
 * void cpumask_clear(cpumask_t *mask);
 *     Zero out all bits.
 *
 * int cpumask_set(cpumask_t *mask, int cpu);
 *     Set bit for cpu. Return 0 on success, -1 if cpu is out of range.
 *
 * int cpumask_unset(cpumask_t *mask, int cpu);
 *     Clear bit for cpu. Return 0 on success, -1 if cpu is out of range.
 *
 * int cpumask_test(const cpumask_t *mask, int cpu);
 *     Return 1 if cpu is set, 0 if not set, -1 if out of range.
 *
 * int cpumask_count(const cpumask_t *mask);
 *     Return the number of set bits (popcount).
 *
 * void cpumask_and(cpumask_t *dst, const cpumask_t *a, const cpumask_t *b);
 *     dst = a AND b.
 *
 * void cpumask_or(cpumask_t *dst, const cpumask_t *a, const cpumask_t *b);
 *     dst = a OR b.
 *
 * int cpumask_first_set(const cpumask_t *mask);
 *     Return the index of the lowest set bit, or -1 if none.
 *
 * int cpumask_next_set(const cpumask_t *mask, int prev);
 *     Return the next set bit after prev, or -1 if none.
 *     This enables iteration: for (cpu = first; cpu >= 0; cpu = next(mask, cpu))
 *
 * void cpumask_print(const cpumask_t *mask);
 *     Print in range format: "0-3,8,12-15"
 */
```

### What They Are Looking For

- Correct word-index and bit-index calculations: `word = cpu / BITS_PER_LONG`, `bit = cpu % BITS_PER_LONG`.
- Use of `__builtin_popcountl` or manual popcount.
- Use of `__builtin_ctzl` (count trailing zeros) for `first_set`/`next_set`, or a manual loop.
- The range-format printer (`cpumask_print`) is a real-world skill. The kernel uses this format in `/sys/devices/system/cpu/online`.
- Boundary handling: cpu 0, cpu 255, cpu 256 (out of range).

---

## Problem 4: Reference-Counted Object

**Time: 20 minutes** | **Category: Memory Management, Kernel Patterns**

### Scenario

Kernel objects (inodes, sk_buffs, kobjects) use reference counting to manage their lifecycle. Implement a generic reference-counted wrapper that frees the object when the last reference is dropped.

### Requirements

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

/*
 * Implement:
 *
 * kref_obj_t *kref_alloc(void *data, destroy_fn destroy);
 *     Create a new ref-counted object with refcount = 1.
 *     The caller is responsible for allocating *data beforehand.
 *     Return NULL on allocation failure.
 *
 * kref_obj_t *kref_get(kref_obj_t *obj);
 *     Increment refcount. Return the object for convenience.
 *     Return NULL if obj is NULL.
 *
 * void kref_put(kref_obj_t **obj_ptr);
 *     Decrement refcount. If it reaches 0, call destroy(data) and free the object.
 *     Set *obj_ptr to NULL after freeing.
 *     Handle NULL gracefully.
 *
 * int kref_count(const kref_obj_t *obj);
 *     Return current refcount, or -1 if obj is NULL.
 */

/* Example usage: */
typedef struct {
    int fd;
    char name[64];
    size_t buffer_size;
    char *buffer;
} device_ctx_t;

void device_ctx_destroy(void *data) {
    device_ctx_t *ctx = (device_ctx_t *)data;
    if (ctx) {
        printf("Destroying device: %s\n", ctx->name);
        free(ctx->buffer);
        free(ctx);
    }
}

/*
 * Write a main() that:
 *   1. Allocates a device_ctx_t on the heap.
 *   2. Wraps it in a kref_obj_t.
 *   3. Takes two additional references (simulating shared ownership).
 *   4. Drops all three references.
 *   5. Verifies the destroy callback was invoked exactly once.
 */
```

### What They Are Looking For

- Clean pointer-to-pointer pattern in `kref_put` to NULL out the caller's handle.
- No double-free, no use-after-free.
- Understanding that in a real kernel this would use `atomic_t` for thread safety.
- Proper NULL checks everywhere.

---

## Problem 5: Find the Bug (Debugging Exercise)

**Time: 15 minutes** | **Category: Debugging, Code Review**

### Scenario

A colleague wrote a function to manage a dynamically growing array of node health records. There are at least 5 bugs. Find them all and explain the fix for each.

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

node_list_t *node_list_create(int initial_capacity) {
    node_list_t *list = malloc(sizeof(node_list_t));
    list->nodes = malloc(initial_capacity * sizeof(node_info_t));  /* BUG? */
    list->count = 0;
    list->capacity = initial_capacity;
    return list;
}

int node_list_add(node_list_t *list, const char *hostname,
                  int cpu_count, double load_avg) {
    if (list->count >= list->capacity) {
        list->capacity *= 2;
        list->nodes = realloc(list->nodes,
                              list->capacity * sizeof(node_info_t));  /* BUG? */
    }

    node_info_t *node = &list->nodes[list->count];
    strcpy(node->hostname, hostname);  /* BUG? */
    node->cpu_count = cpu_count;
    node->load_avg = load_avg;
    node->is_healthy = load_avg < cpu_count;  /* BUG? */

    list->count++;
    return list->count;
}

node_info_t *node_list_find(node_list_t *list, const char *hostname) {
    for (int i = 0; i <= list->count; i++) {  /* BUG? */
        if (strcmp(list->nodes[i].hostname, hostname) == 0)
            return &list->nodes[i];
    }
    return NULL;
}

void node_list_remove(node_list_t *list, int index) {
    if (index < 0 || index > list->count)  /* BUG? */
        return;

    /* Shift remaining elements */
    for (int i = index; i < list->count; i++)
        list->nodes[i] = list->nodes[i + 1];  /* BUG? */

    list->count--;
}

void node_list_destroy(node_list_t *list) {
    free(list->nodes);
    free(list);
}
```

### Your Task

1. List every bug you can find.
2. For each, explain the potential consequence (crash, data corruption, security vulnerability).
3. Write the corrected version of each function.

### Bugs to Find (answer key)

1. **`node_list_create`**: No NULL check on either `malloc`. If `initial_capacity` is 0, the `malloc(0)` behavior is implementation-defined.
2. **`node_list_add` realloc**: The result of `realloc` is assigned directly to `list->nodes`. If `realloc` fails and returns NULL, the original pointer is lost (memory leak) and subsequent access is a NULL dereference. Also, if initial capacity was 0, `capacity *= 2` remains 0.
3. **`node_list_add` strcpy**: No bounds check on `hostname`. If it exceeds 127 characters, this is a buffer overflow. Use `strncpy` with explicit NULL termination.
4. **`node_list_add` is_healthy**: Comparing `load_avg < cpu_count` does implicit int-to-double conversion, which is fine, but the logic is likely wrong. Load average per-core should probably be `load_avg < (double)cpu_count`, or more typically `load_avg / cpu_count < some_threshold`. This is a logic bug, not a crash.
5. **`node_list_find` loop**: `i <= list->count` should be `i < list->count`. Off-by-one reads one element past the valid range.
6. **`node_list_remove` bounds check**: `index > list->count` should be `index >= list->count`. An index equal to count is out of bounds.
7. **`node_list_remove` shift loop**: When `i == list->count - 1`, the copy reads from `list->nodes[list->count]`, which is one past the valid data. The loop should be `i < list->count - 1`.
8. **`node_list_destroy`**: No NULL check on `list`.

---

## Problem 6: Simple Hash Table

**Time: 30 minutes** | **Category: Data Structures**

### Scenario

You are building a lookup table that maps kernel module names to their load status for a fleet management tool. Implement a hash table with separate chaining.

### Requirements

```c
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
 * Implement:
 *
 * unsigned int ht_hash(const char *key, int size);
 *     Hash function. Use djb2 or FNV-1a. Return hash % size.
 *
 * hashtable_t *ht_create(int size);
 *     Allocate a hash table. Return NULL on failure.
 *
 * int ht_insert(hashtable_t *ht, const char *key, int value);
 *     Insert or update. Duplicate the key string (the caller may free theirs).
 *     Return 0 on success, -1 on failure.
 *
 * int ht_lookup(hashtable_t *ht, const char *key, int *value_out);
 *     Look up a key. Write the value to *value_out if found.
 *     Return 0 if found, -1 if not found.
 *
 * int ht_delete(hashtable_t *ht, const char *key);
 *     Remove a key. Free the duplicated key string.
 *     Return 0 if deleted, -1 if not found.
 *
 * void ht_destroy(hashtable_t *ht);
 *     Free everything: all entries, all keys, the bucket array, the table.
 *
 * void ht_print_stats(hashtable_t *ht);
 *     Print: total entries, bucket count, load factor,
 *     longest chain length, number of empty buckets.
 */
```

### What They Are Looking For

- Correct chaining with linked list insert/delete (watch for head-of-list deletion).
- `strdup` for key ownership, with corresponding `free` in delete/destroy.
- A reasonable hash function (not just `key[0] % size`).
- Understanding of load factor and when you might want to resize.
- No memory leaks in any path: insert overwrite, delete, destroy.

---

## Problem 7: Simple Memory Pool Allocator

**Time: 25 minutes** | **Category: Memory Management**

### Scenario

For a high-throughput event processing path, `malloc`/`free` overhead is unacceptable. Implement a fixed-size block memory pool that pre-allocates a chunk of memory and hands out fixed-size blocks. This is the same idea behind the kernel's slab allocator (`kmem_cache`).

### Requirements

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef struct {
    void *memory;           /* the raw allocation */
    void *free_list;        /* pointer to first free block */
    size_t block_size;      /* size of each block (>= sizeof(void*)) */
    int total_blocks;
    int free_count;
} mempool_t;

/*
 * Implement:
 *
 * mempool_t *mempool_create(size_t block_size, int num_blocks);
 *     Allocate a pool. Each block must be at least sizeof(void*) so that
 *     free blocks can store a next-pointer (embedded free list).
 *     Initialize the free list by threading all blocks together.
 *     Return NULL on failure.
 *
 * void *mempool_alloc(mempool_t *pool);
 *     Return a pointer to a free block, or NULL if the pool is exhausted.
 *     O(1) operation: pop from free list head.
 *
 * void mempool_free(mempool_t *pool, void *ptr);
 *     Return a block to the pool. O(1): push onto free list head.
 *     You do NOT need to validate that ptr belongs to this pool
 *     (but mention in your narration that a real implementation would).
 *
 * void mempool_stats(mempool_t *pool);
 *     Print: block_size, total_blocks, free_count, allocated count.
 *
 * void mempool_destroy(mempool_t *pool);
 *     Free the underlying memory and the pool struct.
 */
```

### Key Insight

The free list is embedded in the free blocks themselves. Each free block's first `sizeof(void*)` bytes store a pointer to the next free block. When a block is allocated, its content is available to the user. When freed, the first bytes are reused for the free list pointer.

### What They Are Looking For

- Correct pointer arithmetic to compute block addresses: `(char *)memory + i * block_size`.
- Understanding that `block_size` must be >= `sizeof(void*)`.
- O(1) alloc and free via the embedded free list.
- Clean initialization loop that threads all blocks together.
- Mention of alignment considerations in a real implementation.

---

## Problem 8: Process Exec Argument Builder

**Time: 15 minutes** | **Category: String Handling, Systems Programming**

### Scenario

You are writing a tool that constructs `execv`-style argument vectors from a command string. This is common in process management, container runtimes, and init systems.

### Requirements

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Implement:
 *
 * char **build_argv(const char *cmdline, int *argc_out);
 *     Split cmdline by whitespace into a NULL-terminated argv array.
 *     Each argument string must be independently allocated (strdup).
 *     Set *argc_out to the number of arguments.
 *     Return the argv array, or NULL on error.
 *
 *     Example:
 *       Input:  "  /usr/bin/perf  stat  -e cycles  ./workload  "
 *       Output: argv[0] = "/usr/bin/perf"
 *               argv[1] = "stat"
 *               argv[2] = "-e"
 *               argv[3] = "cycles"
 *               argv[4] = "./workload"
 *               argv[5] = NULL
 *               *argc_out = 5
 *
 * void free_argv(char **argv, int argc);
 *     Free each argument string and the argv array itself.
 *
 * BONUS: Handle simple single-quoted strings (no escaping):
 *     Input:  "grep 'hello world' file.txt"
 *     Output: argv[0] = "grep"
 *             argv[1] = "hello world"
 *             argv[2] = "file.txt"
 */
```

### What They Are Looking For

- Correct `strtok` usage (or manual whitespace-skipping) on a copy of the input (never modify the original).
- Dynamic reallocation of the argv array as you discover more tokens.
- NULL terminator at the end of argv (required by `execv`).
- Clean memory management: if any allocation fails mid-parse, clean up everything already allocated.
- Leading/trailing whitespace and multiple consecutive spaces handled correctly.

---

## General Tips for the Interview

**Before you start coding:**
- Clarify the problem. Repeat it back. Ask about edge cases and error handling expectations.
- Ask whether they want you to write a `main()` with tests, or just the functions.
- Ask if they care about thread safety (likely "not for now, but mention where you would add it").

**While coding:**
- Compile and run frequently. Do not write 80 lines and then compile for the first time.
- Name variables clearly. Use `idx` not `i` when context helps. Use `nr_entries` not `n`.
- Write the function signature first, then the happy path, then error handling.
- When you allocate, immediately think about where the corresponding `free` lives.

**After you finish:**
- Walk through an edge case by hand: empty input, single element, buffer full.
- Point out what you would do differently in production: atomic operations, alignment, error logging, unit tests.
- Connect the problem to real kernel subsystems when natural. You have experience with this at Oracle. Use it.

**C-specific reminders:**
- `strncpy` does not guarantee NULL termination. Always set `buf[sizeof(buf) - 1] = '\0'`.
- `realloc(ptr, new_size)`: always assign to a temp first. If it fails, `ptr` is still valid.
- `sizeof(struct foo)` may include padding. Use it; do not compute sizes by hand.
- Return `-1` or `-errno` for errors, not arbitrary negative values.
- `memset(ptr, 0, size)` after allocation is defensive and often worthwhile.
