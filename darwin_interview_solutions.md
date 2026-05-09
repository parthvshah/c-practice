# Darwin Runtime Engineer (Core OS) - Solutions

Reference solutions in C and C++. Read them after attempting the problem yourself. Each solution includes correctness reasoning, not just code.

---

## Problem 1: Bounded Producer-Consumer Queue

```c
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

struct bounded_queue {
    void   **buf;
    size_t   cap;
    size_t   head;     // pop from here
    size_t   tail;     // push to here
    size_t   count;
    pthread_mutex_t mtx;
    pthread_cond_t  not_full;
    pthread_cond_t  not_empty;
};

bounded_queue_t *bq_create(size_t capacity) {
    bounded_queue_t *q = calloc(1, sizeof(*q));
    if (!q) return NULL;
    q->buf = calloc(capacity, sizeof(void *));
    if (!q->buf) { free(q); return NULL; }
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
    pthread_cond_signal(&q->not_empty);             // signal one waiter, not broadcast
    pthread_mutex_unlock(&q->mtx);
}

void *bq_pop(bounded_queue_t *q) {
    pthread_mutex_lock(&q->mtx);
    while (q->count == 0) {
        pthread_cond_wait(&q->not_empty, &q->mtx);
    }
    void *item = q->buf[q->head];
    q->head = (q->head + 1) % q->cap;
    q->count--;
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->mtx);
    return item;
}
```

**Correctness notes:**
- `while` rather than `if` around the wait is mandatory. Spurious wakeups are real and POSIX permits them.
- Two condvars not one. Signaling `not_empty` after a push wakes a consumer, not another producer that cannot make progress. Cuts wakeup storms.
- `pthread_cond_signal` wakes one waiter. `broadcast` is only needed when the predicate change can satisfy multiple waiters at once (rare here).

---

## Problem 2: Reader-Writer Lock with Writer Preference

```c
#include <pthread.h>

struct rwlock {
    pthread_mutex_t mtx;
    pthread_cond_t  readers_ok;
    pthread_cond_t  writer_ok;
    int active_readers;
    int active_writers;     // 0 or 1
    int waiting_writers;
};

void rwlock_init(rwlock_t *l) {
    pthread_mutex_init(&l->mtx, NULL);
    pthread_cond_init(&l->readers_ok, NULL);
    pthread_cond_init(&l->writer_ok, NULL);
    l->active_readers = l->active_writers = l->waiting_writers = 0;
}

void rwlock_destroy(rwlock_t *l) {
    pthread_mutex_destroy(&l->mtx);
    pthread_cond_destroy(&l->readers_ok);
    pthread_cond_destroy(&l->writer_ok);
}

void rwlock_rdlock(rwlock_t *l) {
    pthread_mutex_lock(&l->mtx);
    // Block new readers if a writer is active or waiting. This prevents writer starvation.
    while (l->active_writers > 0 || l->waiting_writers > 0) {
        pthread_cond_wait(&l->readers_ok, &l->mtx);
    }
    l->active_readers++;
    pthread_mutex_unlock(&l->mtx);
}

void rwlock_rdunlock(rwlock_t *l) {
    pthread_mutex_lock(&l->mtx);
    l->active_readers--;
    if (l->active_readers == 0 && l->waiting_writers > 0) {
        pthread_cond_signal(&l->writer_ok);
    }
    pthread_mutex_unlock(&l->mtx);
}

void rwlock_wrlock(rwlock_t *l) {
    pthread_mutex_lock(&l->mtx);
    l->waiting_writers++;
    while (l->active_readers > 0 || l->active_writers > 0) {
        pthread_cond_wait(&l->writer_ok, &l->mtx);
    }
    l->waiting_writers--;
    l->active_writers = 1;
    pthread_mutex_unlock(&l->mtx);
}

void rwlock_wrunlock(rwlock_t *l) {
    pthread_mutex_lock(&l->mtx);
    l->active_writers = 0;
    if (l->waiting_writers > 0) {
        pthread_cond_signal(&l->writer_ok);     // hand off to next writer
    } else {
        pthread_cond_broadcast(&l->readers_ok); // let a batch of readers in
    }
    pthread_mutex_unlock(&l->mtx);
}
```

**Correctness notes:**
- The `waiting_writers > 0` check in `rwlock_rdlock` is the writer-preference guarantee. Without it, a steady stream of readers can starve writers indefinitely.
- On unlock the writer broadcasts to readers, since multiple readers can proceed concurrently. To another writer it signals one.
- Upgradable variants need two-phase intent locks. Naive upgrade deadlocks if two readers attempt simultaneous upgrade.

---

## Problem 3: Lock-Free SPSC Ring Buffer

```c
#include <stdatomic.h>
#include <stdlib.h>
#include <stdbool.h>

#define CACHELINE 64

struct spsc_ring {
    void   **buf;
    size_t   mask;          // capacity - 1; capacity is power of 2

    // Pad to avoid producer and consumer indices sharing a cache line.
    _Alignas(CACHELINE) atomic_size_t head;   // written by consumer
    _Alignas(CACHELINE) atomic_size_t tail;   // written by producer
};

spsc_ring_t *spsc_create(size_t capacity) {
    if (capacity == 0 || (capacity & (capacity - 1)) != 0) return NULL;
    spsc_ring_t *r = aligned_alloc(CACHELINE, sizeof(*r));
    if (!r) return NULL;
    r->buf = calloc(capacity, sizeof(void *));
    if (!r->buf) { free(r); return NULL; }
    r->mask = capacity - 1;
    atomic_store_explicit(&r->head, 0, memory_order_relaxed);
    atomic_store_explicit(&r->tail, 0, memory_order_relaxed);
    return r;
}

void spsc_destroy(spsc_ring_t *r) { free(r->buf); free(r); }

bool spsc_push(spsc_ring_t *r, void *item) {
    size_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
    size_t head = atomic_load_explicit(&r->head, memory_order_acquire);
    if (tail - head > r->mask) return false;        // full
    r->buf[tail & r->mask] = item;
    atomic_store_explicit(&r->tail, tail + 1, memory_order_release);
    return true;
}

bool spsc_pop(spsc_ring_t *r, void **out) {
    size_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
    size_t tail = atomic_load_explicit(&r->tail, memory_order_acquire);
    if (head == tail) return false;                  // empty
    *out = r->buf[head & r->mask];
    atomic_store_explicit(&r->head, head + 1, memory_order_release);
    return true;
}
```

**Memory ordering justification:**
- Producer's load of its own `tail` is `relaxed`. No other thread writes it.
- Producer's load of `head` is `acquire`. It pairs with the consumer's `release` store on `head`. This guarantees the producer sees the slot as freed before reusing it.
- Producer's store on `tail` is `release`. The consumer's `acquire` load on `tail` then synchronizes-with this store. The data write to `buf[tail & mask]` happens-before the consumer reads it.
- Symmetric for the consumer side.

**Why ARM matters:** x86 has TSO, so most loads are effectively acquire and stores are effectively release. ARM does not. Forgetting the acquire and release on ARM produces a working program on a Mac mini Intel and a randomly broken one on an M-series chip. State this in the interview.

**False sharing:** the cache-line alignment on `head` and `tail` is not optional. Without it, every producer write invalidates the consumer's cache line and throughput drops by an order of magnitude.

---

## Problem 4: Deadlock Detection from a Lock Graph

```c
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int thread_id;
    int holds_lock_id;
    int waits_for_lock_id;
} lock_edge_t;

// Build wait-for graph: thread T1 -> thread T2 if T1 waits for a lock T2 holds.
// Cycle in this graph == deadlock.

enum { WHITE = 0, GRAY = 1, BLACK = 2 };

static bool dfs(int u, int *adj_start, int *adj_list, int *color,
                int *parent, int *cycle_start) {
    color[u] = GRAY;
    for (int i = adj_start[u]; i < adj_start[u + 1]; i++) {
        int v = adj_list[i];
        if (color[v] == GRAY) {
            *cycle_start = v;
            parent[v] = parent[v] == -1 ? u : parent[v];
            return true;
        }
        if (color[v] == WHITE) {
            parent[v] = u;
            if (dfs(v, adj_start, adj_list, color, parent, cycle_start)) return true;
        }
    }
    color[u] = BLACK;
    return false;
}

bool detect_deadlock(const lock_edge_t *edges, size_t n,
                     int *cycle_out, size_t *cycle_len) {
    // Map lock_id -> holder thread_id
    int max_thread = 0, max_lock = 0;
    for (size_t i = 0; i < n; i++) {
        if (edges[i].thread_id > max_thread) max_thread = edges[i].thread_id;
        if (edges[i].holds_lock_id > max_lock) max_lock = edges[i].holds_lock_id;
        if (edges[i].waits_for_lock_id > max_lock) max_lock = edges[i].waits_for_lock_id;
    }
    int *holder = malloc(sizeof(int) * (max_lock + 2));
    for (int i = 0; i <= max_lock + 1; i++) holder[i] = -1;
    for (size_t i = 0; i < n; i++) {
        if (edges[i].holds_lock_id >= 0) holder[edges[i].holds_lock_id] = edges[i].thread_id;
    }

    // Build CSR adjacency for threads.
    int V = max_thread + 1;
    int *deg = calloc(V + 1, sizeof(int));
    for (size_t i = 0; i < n; i++) {
        int w = edges[i].waits_for_lock_id;
        if (w >= 0 && holder[w] >= 0 && holder[w] != edges[i].thread_id) {
            deg[edges[i].thread_id + 1]++;
        }
    }
    for (int i = 1; i <= V; i++) deg[i] += deg[i - 1];
    int *adj_start = deg;
    int *adj_list = malloc(sizeof(int) * (adj_start[V] > 0 ? adj_start[V] : 1));
    int *cursor = calloc(V, sizeof(int));
    for (size_t i = 0; i < n; i++) {
        int w = edges[i].waits_for_lock_id;
        if (w >= 0 && holder[w] >= 0 && holder[w] != edges[i].thread_id) {
            int u = edges[i].thread_id;
            adj_list[adj_start[u] + cursor[u]++] = holder[w];
        }
    }

    int *color = calloc(V, sizeof(int));
    int *parent = malloc(sizeof(int) * V);
    for (int i = 0; i < V; i++) parent[i] = -1;
    int cycle_start = -1;
    bool found = false;
    for (int u = 0; u < V && !found; u++) {
        if (color[u] == WHITE && dfs(u, adj_start, adj_list, color, parent, &cycle_start)) {
            found = true;
        }
    }

    if (found && cycle_out && cycle_len) {
        // Walk parent pointers from cycle_start until we loop back.
        size_t k = 0;
        int cur = cycle_start;
        do {
            cycle_out[k++] = cur;
            cur = parent[cur];
        } while (cur != cycle_start && cur != -1 && k < (size_t)V);
        cycle_out[k++] = cycle_start;
        *cycle_len = k;
    }

    free(holder); free(adj_start); free(adj_list); free(cursor);
    free(color); free(parent);
    return found;
}
```

**Notes:**
- Wait-for graph is the standard reduction. Three-color DFS is O(V+E).
- Linux's `lockdep` does the same idea but on lock-class graphs at acquire time, not at panic time, and tracks irq-safe vs irq-unsafe contexts. Mention this.

---

## Problem 5: Slab Allocator

```c
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include <pthread.h>

#define SLAB_PAGES 4   // 4 pages per slab; tune per object size

typedef struct slab {
    struct slab *next;
    void  *base;
    size_t free_count;
    void  *free_list;       // singly linked through free objects
} slab_t;

struct slab_cache {
    size_t object_size;     // already rounded up
    size_t align;
    size_t slab_bytes;
    size_t per_slab;
    slab_t *partial;        // slabs with at least one free object
    slab_t *full;           // slabs with zero free
    pthread_mutex_t mtx;
};

static size_t round_up(size_t v, size_t a) { return (v + a - 1) & ~(a - 1); }

slab_cache_t *slab_create(size_t object_size, size_t align) {
    if (align < sizeof(void *)) align = sizeof(void *);
    slab_cache_t *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->object_size = round_up(object_size, align);
    c->align = align;
    long pg = sysconf(_SC_PAGESIZE);
    c->slab_bytes = (size_t)pg * SLAB_PAGES;
    c->per_slab = c->slab_bytes / c->object_size;
    pthread_mutex_init(&c->mtx, NULL);
    return c;
}

static slab_t *grow_locked(slab_cache_t *c) {
    void *mem = mmap(NULL, c->slab_bytes, PROT_READ | PROT_WRITE,
                     MAP_ANON | MAP_PRIVATE, -1, 0);
    if (mem == MAP_FAILED) return NULL;
    slab_t *s = malloc(sizeof(*s));
    if (!s) { munmap(mem, c->slab_bytes); return NULL; }
    s->base = mem;
    s->free_count = c->per_slab;

    // Thread the free list through the objects themselves.
    char *p = mem;
    void *prev = NULL;
    for (size_t i = 0; i < c->per_slab; i++) {
        void **slot = (void **)p;
        *slot = prev;
        prev = slot;
        p += c->object_size;
    }
    s->free_list = prev;     // points to last initialized; actually points to chain head
    s->next = c->partial;
    c->partial = s;
    return s;
}

void *slab_alloc(slab_cache_t *c) {
    pthread_mutex_lock(&c->mtx);
    if (!c->partial && !grow_locked(c)) {
        pthread_mutex_unlock(&c->mtx);
        return NULL;
    }
    slab_t *s = c->partial;
    void **head = s->free_list;
    s->free_list = *head;
    s->free_count--;

    if (s->free_count == 0) {
        // Move to full list.
        c->partial = s->next;
        s->next = c->full;
        c->full = s;
    }
    pthread_mutex_unlock(&c->mtx);
    return head;
}

void slab_free(slab_cache_t *c, void *p) {
    pthread_mutex_lock(&c->mtx);
    // Find the owning slab. For real code use a hash table or stuff slab pointer
    // in the trailing bytes of each page. Linear scan here for clarity.
    slab_t **prev = &c->full;
    slab_t *s = c->full;
    while (s) {
        if ((char *)p >= (char *)s->base && (char *)p < (char *)s->base + c->slab_bytes) {
            *prev = s->next;
            s->next = c->partial;
            c->partial = s;
            break;
        }
        prev = &s->next;
        s = s->next;
    }
    if (!s) {
        s = c->partial;
        while (s) {
            if ((char *)p >= (char *)s->base && (char *)p < (char *)s->base + c->slab_bytes) break;
            s = s->next;
        }
    }
    void **slot = p;
    *slot = s->free_list;
    s->free_list = slot;
    s->free_count++;
    pthread_mutex_unlock(&c->mtx);
}
```

**Notes:**
- Free objects double as their own list nodes. No external bitmap or per-object metadata.
- Real allocators encode the owning slab pointer at the start or end of each page so `free` is O(1). The linear scan above is a teaching simplification.
- Per-CPU magazines (Bonwick's 2001 paper) eliminate the global lock on the hot path. XNU's zone allocator does the same.
- For UAF detection, on free overwrite the object with a poison pattern and on alloc verify the poison. Or use guard pages around each slab.

---

## Problem 6: Free-List Allocator with Coalescing

```c
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define ALIGN 16
#define HDR_SIZE  (sizeof(block_hdr_t))

typedef struct block_hdr {
    size_t size;            // block size including header and footer; low bit = allocated
    struct block_hdr *prev_free;
    struct block_hdr *next_free;
} block_hdr_t;

typedef struct {
    size_t size;            // duplicate of header.size for backward coalesce
} block_ftr_t;

struct heap {
    void *base;
    size_t size;
    block_hdr_t *free_head;
};

static size_t pack(size_t sz, bool alloc) { return sz | (alloc ? 1 : 0); }
static size_t blk_size(const block_hdr_t *b) { return b->size & ~(size_t)1; }
static bool   blk_alloc(const block_hdr_t *b) { return b->size & 1; }
static block_ftr_t *footer(block_hdr_t *b) {
    return (block_ftr_t *)((char *)b + blk_size(b) - sizeof(block_ftr_t));
}
static block_hdr_t *next_block(heap_t *h, block_hdr_t *b) {
    char *p = (char *)b + blk_size(b);
    if (p >= (char *)h->base + h->size) return NULL;
    return (block_hdr_t *)p;
}
static block_hdr_t *prev_block(heap_t *h, block_hdr_t *b) {
    if ((char *)b == (char *)h->base) return NULL;
    block_ftr_t *pf = (block_ftr_t *)((char *)b - sizeof(block_ftr_t));
    return (block_hdr_t *)((char *)b - (pf->size & ~(size_t)1));
}

static size_t round_up(size_t v, size_t a) { return (v + a - 1) & ~(a - 1); }

static void list_remove(heap_t *h, block_hdr_t *b) {
    if (b->prev_free) b->prev_free->next_free = b->next_free;
    else h->free_head = b->next_free;
    if (b->next_free) b->next_free->prev_free = b->prev_free;
}
static void list_push(heap_t *h, block_hdr_t *b) {
    b->prev_free = NULL;
    b->next_free = h->free_head;
    if (h->free_head) h->free_head->prev_free = b;
    h->free_head = b;
}

heap_t *heap_create(void *region, size_t size) {
    heap_t *h = region;
    char *region_after = (char *)region + sizeof(heap_t);
    region_after = (char *)round_up((uintptr_t)region_after, ALIGN);
    size_t usable = size - (region_after - (char *)region);
    h->base = region_after;
    h->size = usable & ~(size_t)(ALIGN - 1);

    block_hdr_t *b = h->base;
    b->size = pack(h->size, false);
    footer(b)->size = b->size;
    b->prev_free = b->next_free = NULL;
    h->free_head = b;
    return h;
}

void *heap_alloc(heap_t *h, size_t size) {
    size_t need = round_up(size + sizeof(block_hdr_t) + sizeof(block_ftr_t), ALIGN);
    if (need < sizeof(block_hdr_t) + sizeof(block_ftr_t))
        need = sizeof(block_hdr_t) + sizeof(block_ftr_t);

    // First fit. Best fit is one extra comparison per node; use it if fragmentation matters.
    block_hdr_t *b = h->free_head;
    while (b && blk_size(b) < need) b = b->next_free;
    if (!b) return NULL;

    list_remove(h, b);
    size_t leftover = blk_size(b) - need;
    if (leftover >= sizeof(block_hdr_t) + sizeof(block_ftr_t) + ALIGN) {
        b->size = pack(need, true);
        footer(b)->size = b->size;
        block_hdr_t *split = next_block(h, b);
        split->size = pack(leftover, false);
        footer(split)->size = split->size;
        list_push(h, split);
    } else {
        b->size = pack(blk_size(b), true);
        footer(b)->size = b->size;
    }
    return (char *)b + sizeof(block_hdr_t);
}

void heap_free(heap_t *h, void *p) {
    if (!p) return;
    block_hdr_t *b = (block_hdr_t *)((char *)p - sizeof(block_hdr_t));
    b->size = pack(blk_size(b), false);
    footer(b)->size = b->size;

    block_hdr_t *prev = prev_block(h, b);
    block_hdr_t *next = next_block(h, b);

    if (next && !blk_alloc(next)) {
        list_remove(h, next);
        b->size = pack(blk_size(b) + blk_size(next), false);
        footer(b)->size = b->size;
    }
    if (prev && !blk_alloc(prev)) {
        list_remove(h, prev);
        prev->size = pack(blk_size(prev) + blk_size(b), false);
        footer(prev)->size = prev->size;
        b = prev;
    }
    list_push(h, b);
}
```

**Notes:**
- Boundary tags (the duplicate footer) are what make backward coalesce O(1). Without them, you would need to walk the free list or a separate allocated-block index.
- Splitting only when leftover is large enough avoids creating unusable shards.
- libmalloc uses size-segregated allocators (tiny under 1 KB, small under 32 KB, large above) with different strategies per class. Mention this when comparing.

---

## Problem 7: Thread-Safe Reference-Counted Object

```c
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

typedef void (*rc_dtor_t)(void *);

struct refcounted {
    atomic_int   count;
    rc_dtor_t    dtor;
    size_t       payload_size;
    // payload follows
};

refcounted_t *rc_alloc(size_t payload_size, rc_dtor_t dtor) {
    refcounted_t *r = malloc(sizeof(*r) + payload_size);
    if (!r) return NULL;
    atomic_init(&r->count, 1);
    r->dtor = dtor;
    r->payload_size = payload_size;
    return r;
}

void *rc_payload(refcounted_t *r) {
    return (char *)r + sizeof(*r);
}

void rc_retain(refcounted_t *r) {
    atomic_fetch_add_explicit(&r->count, 1, memory_order_relaxed);
}

void rc_release(refcounted_t *r) {
    if (atomic_fetch_sub_explicit(&r->count, 1, memory_order_release) == 1) {
        atomic_thread_fence(memory_order_acquire);
        if (r->dtor) r->dtor(rc_payload(r));
        free(r);
    }
}
```

**Memory ordering justification:**
- `retain` is `relaxed`. The caller already holds a reference, so the object is alive. No new ordering constraint is established by incrementing.
- `release` decrement is `memory_order_release`. All writes the current thread did to the object before releasing must be visible to the thread that performs the destruction.
- The fence-after-decrement pattern: `release` on the decrement, then `acquire` fence inside the `count == 1` branch. This avoids the cost of `acq_rel` on every release while ensuring the destroying thread synchronizes with all prior releasers. Boost's `shared_ptr` and libc++ both use this pattern.
- ObjC ARC uses similar atomics through `os_object` and `objc_retain` / `objc_release`. Look at `_objc_rootRetain` in objc4 source.

---

## Problem 8: Thread Pool with Work Stealing

Simpler mutex-protected deque variant. State the tradeoff: a true Chase-Lev deque is lock-free between owner pushes/pops and stealer steals, but is subtle on weak memory models. For an interview, this implementation is correct and demonstrates the architecture; mention Chase-Lev as the optimization.

```cpp
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

class ThreadPool {
public:
    explicit ThreadPool(size_t num_workers) : stop_(false), pending_(0) {
        workers_.reserve(num_workers);
        queues_.resize(num_workers);
        for (size_t i = 0; i < num_workers; i++) {
            workers_.emplace_back([this, i] { worker_loop(i); });
        }
    }

    ~ThreadPool() {
        {
            std::lock_guard<std::mutex> g(global_mtx_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto &t : workers_) t.join();
    }

    void submit(std::function<void()> task) {
        size_t idx = next_.fetch_add(1, std::memory_order_relaxed) % queues_.size();
        {
            std::lock_guard<std::mutex> g(queues_[idx].mtx);
            queues_[idx].tasks.push_back(std::move(task));
        }
        pending_.fetch_add(1, std::memory_order_relaxed);
        cv_.notify_one();
    }

    void wait_idle() {
        std::unique_lock<std::mutex> lk(global_mtx_);
        idle_cv_.wait(lk, [this] {
            return pending_.load(std::memory_order_acquire) == 0;
        });
    }

private:
    struct Queue {
        std::mutex mtx;
        std::deque<std::function<void()>> tasks;
    };

    bool pop_local(size_t i, std::function<void()> &out) {
        std::lock_guard<std::mutex> g(queues_[i].mtx);
        if (queues_[i].tasks.empty()) return false;
        out = std::move(queues_[i].tasks.back());      // owner pops back (LIFO, hot cache)
        queues_[i].tasks.pop_back();
        return true;
    }

    bool steal_from(size_t victim, std::function<void()> &out) {
        std::lock_guard<std::mutex> g(queues_[victim].mtx);
        if (queues_[victim].tasks.empty()) return false;
        out = std::move(queues_[victim].tasks.front()); // stealer takes front (FIFO)
        queues_[victim].tasks.pop_front();
        return true;
    }

    void worker_loop(size_t self) {
        std::mt19937 rng(std::random_device{}() + self);
        while (true) {
            std::function<void()> task;
            if (pop_local(self, task) || try_steal(self, rng, task)) {
                task();
                if (pending_.fetch_sub(1, std::memory_order_release) == 1) {
                    std::lock_guard<std::mutex> g(global_mtx_);
                    idle_cv_.notify_all();
                }
                continue;
            }
            std::unique_lock<std::mutex> lk(global_mtx_);
            cv_.wait_for(lk, std::chrono::milliseconds(1), [this] {
                return stop_ || pending_.load(std::memory_order_acquire) > 0;
            });
            if (stop_ && pending_.load() == 0) return;
        }
    }

    bool try_steal(size_t self, std::mt19937 &rng, std::function<void()> &out) {
        size_t n = queues_.size();
        for (size_t k = 0; k < n; k++) {
            size_t v = (self + 1 + rng() % (n - 1 ? n - 1 : 1)) % n;
            if (v == self) continue;
            if (steal_from(v, out)) return true;
        }
        return false;
    }

    std::vector<std::thread> workers_;
    std::vector<Queue>       queues_;
    std::atomic<size_t>      next_{0};
    std::atomic<size_t>      pending_;
    std::mutex               global_mtx_;
    std::condition_variable  cv_;
    std::condition_variable  idle_cv_;
    bool                     stop_;
};
```

**Notes:**
- Owner pops the back, stealer takes the front. This minimizes contention because they touch opposite ends of the deque and improves cache locality for the owner (recently pushed work is often hot).
- True Chase-Lev avoids the per-deque mutex by using a circular buffer with atomic top and bottom indices and a CAS protocol. The subtlety on ARM is that the steal path needs an `acquire` on top and a `relaxed` load of bottom, with a CAS that establishes the steal. Get this wrong and you get torn reads or double-execution under heavy contention.
- libdispatch's concurrent queues use a more sophisticated scheme with priority bands and QoS propagation, not a flat work-stealing pool. Worth mentioning.

---

## Problem 9: Lock-Free MPMC Queue (Michael-Scott)

```c
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>

typedef struct node {
    void *value;
    _Atomic(struct node *) next;
} node_t;

struct mpmc_queue {
    _Atomic(node_t *) head;
    _Atomic(node_t *) tail;
};

mpmc_queue_t *mpmc_create(void) {
    mpmc_queue_t *q = malloc(sizeof(*q));
    node_t *dummy = malloc(sizeof(*dummy));
    dummy->value = NULL;
    atomic_init(&dummy->next, NULL);
    atomic_init(&q->head, dummy);
    atomic_init(&q->tail, dummy);
    return q;
}

void mpmc_enqueue(mpmc_queue_t *q, void *item) {
    node_t *n = malloc(sizeof(*n));
    n->value = item;
    atomic_init(&n->next, NULL);

    for (;;) {
        node_t *tail = atomic_load_explicit(&q->tail, memory_order_acquire);
        node_t *next = atomic_load_explicit(&tail->next, memory_order_acquire);
        if (tail != atomic_load_explicit(&q->tail, memory_order_acquire)) continue;
        if (next != NULL) {
            // Tail was lagging. Help advance.
            atomic_compare_exchange_weak_explicit(&q->tail, &tail, next,
                memory_order_release, memory_order_relaxed);
            continue;
        }
        // Try to link our node.
        if (atomic_compare_exchange_weak_explicit(&tail->next, &next, n,
                memory_order_release, memory_order_relaxed)) {
            // Linked. Try to swing tail. Failure is fine; another thread will help.
            atomic_compare_exchange_strong_explicit(&q->tail, &tail, n,
                memory_order_release, memory_order_relaxed);
            return;
        }
    }
}

bool mpmc_dequeue(mpmc_queue_t *q, void **out) {
    for (;;) {
        node_t *head = atomic_load_explicit(&q->head, memory_order_acquire);
        node_t *tail = atomic_load_explicit(&q->tail, memory_order_acquire);
        node_t *next = atomic_load_explicit(&head->next, memory_order_acquire);
        if (head != atomic_load_explicit(&q->head, memory_order_acquire)) continue;
        if (head == tail) {
            if (next == NULL) return false;     // empty
            // Tail lagging; help.
            atomic_compare_exchange_weak_explicit(&q->tail, &tail, next,
                memory_order_release, memory_order_relaxed);
            continue;
        }
        void *value = next->value;
        if (atomic_compare_exchange_weak_explicit(&q->head, &head, next,
                memory_order_release, memory_order_relaxed)) {
            *out = value;
            // WARNING: cannot free(head) here without a safe-memory-reclamation scheme.
            // See discussion below.
            return true;
        }
    }
}
```

**ABA discussion:**
- The classic MS queue assumes nodes are not reused. If you `free(head)` immediately and a subsequent allocation returns the same address, a stalled thread holding an old `head` pointer can succeed in CAS against a node that is logically a different one. Crash or data loss.
- Three standard fixes:
  1. **Tagged pointers / DCAS:** pack a 64-bit pointer with a 64-bit version counter and CAS the 128-bit value (`cmpxchg16b` on x86-64, `CASP` on ARMv8.1). Every release bumps the counter, so ABA cannot happen within `2^64` operations. Apple Silicon supports this via LSE atomics.
  2. **Hazard pointers (Maged Michael, 2004):** each thread publishes the pointers it is about to dereference. Reclaimers check published hazards before freeing.
  3. **Epoch-based reclamation (EBR):** threads enter and exit critical regions; memory is freed only after all threads have left the epoch in which it was retired. RCU is a kernel-side specialization of this idea.

**Comparison:**
- Hazard pointers: bounded memory, lock-free, per-access overhead.
- EBR: low per-access cost, unbounded memory if a thread stalls in a critical region.
- RCU: ideal for read-mostly kernel data structures. XNU has its own variants in some subsystems.

---

## Problem 10: IPC Tradeoff Sketch

For 1 KB messages at 100 K/sec:

| Mechanism | Latency (typical) | Throughput | Security | Complexity |
|---|---|---|---|---|
| Pipe | 5 to 20 us per message | Limited by 2 syscalls per send | Kernel-mediated, fd-based | Trivial |
| POSIX shm + futex | Sub-microsecond on cache hit | Hundreds of MB/s | Both processes need shm access; bug in one corrupts the other | Moderate, must hand-roll a ring buffer |
| Mach port | 1 to 5 us | Several hundred K/sec | Capability-based, kernel-enforced; ports cannot be forged | High; Mach API is unfamiliar territory |

**When each wins:**
- **Pipe:** you do not control both endpoints, or the rate is low and code simplicity matters.
- **Shared memory:** maximum throughput, both processes are trusted, you can amortize syncrhonization with batched notifications.
- **Mach port:** required for talking to system services on Darwin; security boundary between mutually distrusting processes; structured message types and rights transfer.

Sketch the shared-memory ring buffer if pushed: it is Problem 3's SPSC ring placed in `shm_open`-backed memory, with a pair of futex-equivalent wakeups (`__ulock_wait` / `__ulock_wake` on Darwin) when one side blocks.

---

## Debugging Scenarios

### Scenario A: 4 MB/hr RSS growth, leaks reports nothing
- `leaks` finds unreachable allocations. A growing reachable allocation (cache without eviction, retained array) is invisible to it.
- Methodology:
  1. `vmmap -summary <pid>` over time. Identify which region category grows. Heap, mapped file, MALLOC_LARGE?
  2. `heap <pid>` to see allocation count and total per class.
  3. `malloc_history` with `MallocStackLogging=1` to attribute growth to a stack.
  4. If growth is in `MALLOC_LARGE`, check for image or buffer caches.
  5. Instruments Allocations with a Mark Generation diff between two snapshots an hour apart.

### Scenario B: weekly nil deref from libdispatch callback
- "Once per week" plus dispatch callback strongly suggests a use-after-free where an object was `release`d while a block capturing it was still queued, then dequeued after free.
- Methodology:
  1. Enable Malloc Scribble (`MallocScribble=1`) and Guard Malloc to crash earlier and closer to root cause.
  2. Enable zombie objects on the suspect class to convert UAF into a clean message-to-zombie diagnostic.
  3. Inspect lifetime model around the captured object. Is the block holding a strong reference? Is it weak and getting nilled mid-callback?
  4. Run with TSan if accesses cross threads. Race on the refcount or backing pointer is the next hypothesis.

### Scenario C: 100 ms turning into 140 ms post-update, identical CPU%
- Same CPU utilization but worse wall time means something is stretching out without the kernel sleeping. Candidates: lock contention, increased cache misses, lower frequency, scheduling on E-cores, TLB thrash.
- Methodology:
  1. `sample` or Instruments Time Profiler to compare hot stacks before and after.
  2. `powermetrics` to confirm P-core vs E-core residency and frequency.
  3. `kperf` or DTrace to count cycles, instructions, cache misses, branch mispredicts. A change in IPC fingerprints microarchitectural regression.
  4. Look for new locking introduced in the update. `lockstat` provider in DTrace.
  5. Bisect kernel commits if you have the build infrastructure. Mention the Codex-driven bisection from your Oracle work; it is directly applicable.

### Scenario D: lock-ordering panic, prevention going forward
Beyond the immediate fix:
- Adopt a kernel-wide lock ordering policy. Document partial order; statically check it.
- Enable lockdep-style runtime checking in debug builds. Each lock has a class; the runtime tracks acquired-before relations and panics on the first violation, not on actual deadlock. This catches the bug long before it deadlocks in production.
- Static analysis: annotate locks with capability attributes (Clang's thread safety analysis) so the compiler flags violations at build time.
- Code review checklist that flags any function acquiring two or more locks for explicit ordering justification.
