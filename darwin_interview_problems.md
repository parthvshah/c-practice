# Darwin Runtime Engineer (Core OS) - Coding Problems

Practice problems mapped to the 5-day study plan. Time each one. Code on paper or in a plain editor without autocomplete to simulate interview conditions.

---

## Concurrency and Synchronization

### Problem 1: Bounded Producer-Consumer Queue
Implement a thread-safe bounded FIFO queue supporting multiple producers and multiple consumers. Block producers when full, block consumers when empty.

**Required interface (C):**
```c
typedef struct bounded_queue bounded_queue_t;

bounded_queue_t *bq_create(size_t capacity);
void bq_destroy(bounded_queue_t *q);
void bq_push(bounded_queue_t *q, void *item);     // blocks if full
void *bq_pop(bounded_queue_t *q);                  // blocks if empty
```

**Constraints:**
- Use pthread mutex and condition variables.
- Wake exactly the threads that can make progress. No thundering herd.
- Handle spurious wakeups correctly.

**Follow-ups to expect:**
- How would you add a non-blocking `bq_try_pop`?
- How would you add a timed wait?
- Convert to lock-free for SPSC. What changes?

---

### Problem 2: Reader-Writer Lock with Writer Preference
Implement an rwlock from scratch using only a mutex and condvars. Multiple readers concurrent, single writer exclusive, writers must not be starved by a steady stream of readers.

**Required interface (C):**
```c
typedef struct rwlock rwlock_t;

void rwlock_init(rwlock_t *l);
void rwlock_destroy(rwlock_t *l);
void rwlock_rdlock(rwlock_t *l);
void rwlock_rdunlock(rwlock_t *l);
void rwlock_wrlock(rwlock_t *l);
void rwlock_wrunlock(rwlock_t *l);
```

**Constraints:**
- A waiting writer must block new readers from acquiring the lock.
- No deadlock when a writer waits while readers drain.

**Follow-ups:**
- How would you make this upgradable (reader to writer)? What deadlock case must you handle?
- Compare against pthread_rwlock_t. When would you write your own?

---

### Problem 3: Lock-Free SPSC Ring Buffer
Single-producer single-consumer ring buffer using C11 atomics. No locks, no kernel calls on the fast path.

**Required interface (C):**
```c
typedef struct spsc_ring spsc_ring_t;

spsc_ring_t *spsc_create(size_t capacity);  // capacity must be power of 2
void spsc_destroy(spsc_ring_t *r);
bool spsc_push(spsc_ring_t *r, void *item);   // returns false if full
bool spsc_pop(spsc_ring_t *r, void **out);    // returns false if empty
```

**Constraints:**
- Use `atomic_load_explicit` and `atomic_store_explicit` with the minimum memory ordering required for correctness.
- Justify every use of acquire, release, and relaxed.
- Avoid false sharing between producer and consumer indices.

**Follow-ups:**
- Why does this break for MPMC?
- How does ARM's weaker memory model affect this versus x86?

---

### Problem 4: Deadlock Detection from a Lock Graph
Given a list of threads and the locks each thread currently holds plus the lock each thread is waiting on, detect whether the system is deadlocked. If yes, print one cycle.

**Required interface (C):**
```c
// edges[i] = {thread_id, holds_lock_id, waits_for_lock_id}
// A thread waiting on no lock has waits_for_lock_id == -1.
typedef struct {
    int thread_id;
    int holds_lock_id;
    int waits_for_lock_id;
} lock_edge_t;

bool detect_deadlock(const lock_edge_t *edges, size_t n, int *cycle_out, size_t *cycle_len);
```

**Constraints:**
- Build a wait-for graph and detect cycles using DFS with three-color marking.
- Return any one cycle, not all.

**Follow-ups:**
- How would you scale this to millions of threads?
- How does the kernel's lockdep do this in real time?

---

## Memory Management

### Problem 5: Slab Allocator
Implement a slab allocator for a fixed object size. Backing memory comes from `mmap` in page-sized chunks. Track free objects with an embedded free list.

**Required interface (C):**
```c
typedef struct slab_cache slab_cache_t;

slab_cache_t *slab_create(size_t object_size, size_t align);
void slab_destroy(slab_cache_t *c);
void *slab_alloc(slab_cache_t *c);
void slab_free(slab_cache_t *c, void *p);
```

**Constraints:**
- Free objects must store the next-pointer in their own memory. No external metadata per object.
- Grow the cache by adding new slabs when all existing slabs are full.
- Bonus: thread safety with a per-cache lock, then per-CPU magazines.

**Follow-ups:**
- Compare to XNU's `zalloc` zone allocator.
- How would you add guard pages and poisoning for use-after-free detection?

---

### Problem 6: Free-List Allocator with Coalescing
Implement `malloc` and `free` over a fixed-size heap region. Track free blocks in a doubly linked list. Coalesce adjacent free blocks on `free`.

**Required interface (C):**
```c
typedef struct heap heap_t;

heap_t *heap_create(void *region, size_t size);
void *heap_alloc(heap_t *h, size_t size);
void heap_free(heap_t *h, void *p);
```

**Constraints:**
- Use boundary tags so coalescing with the previous block is O(1).
- First-fit or best-fit, your choice. Justify it.
- Round allocations to a sensible alignment (16 bytes).

**Follow-ups:**
- How would you reduce fragmentation? Segregated free lists?
- How does this compare to libmalloc's tiny, small, and large allocators?

---

### Problem 7: Thread-Safe Reference-Counted Object
Implement a refcounted object where `retain` and `release` are safe to call from any thread, and the destructor runs exactly once when the count reaches zero.

**Required interface (C):**
```c
typedef struct refcounted refcounted_t;

refcounted_t *rc_alloc(size_t payload_size, void (*dtor)(void *));
void *rc_payload(refcounted_t *r);
void rc_retain(refcounted_t *r);
void rc_release(refcounted_t *r);
```

**Constraints:**
- Use C11 atomics, not a mutex.
- Justify the memory ordering on retain and release.
- The destructor must observe all writes made by threads that previously held a reference.

**Follow-ups:**
- Why is `memory_order_relaxed` correct on retain but not on release?
- What is the standard fence-after-decrement pattern and why?
- How does this compare to Apple's `os_object` and ObjC ARC?

---

## Systems Design

### Problem 8: Thread Pool with Work Stealing
Design and implement a fixed-size thread pool where each worker has its own deque of tasks. Workers push and pop from the bottom of their own deque. Idle workers steal from the top of another worker's deque.

**Required interface (C++):**
```cpp
class ThreadPool {
public:
    explicit ThreadPool(size_t num_workers);
    ~ThreadPool();
    void submit(std::function<void()> task);
    void wait_idle();
};
```

**Constraints:**
- Use a Chase-Lev style deque or a simpler mutex-protected deque if pressed for time. State the tradeoff.
- Submitting from outside the pool should round-robin or pick a random worker.
- Clean shutdown: drain remaining tasks then join.

**Follow-ups:**
- Why is the Chase-Lev deque hard to get right on ARM?
- How does this map onto libdispatch's concurrent queues?

---

### Problem 9: Lock-Free MPMC Queue
Implement a multi-producer multi-consumer queue using CAS. Address the ABA problem.

**Required interface (C):**
```c
typedef struct mpmc_queue mpmc_queue_t;

mpmc_queue_t *mpmc_create(void);
void mpmc_destroy(mpmc_queue_t *q);
void mpmc_enqueue(mpmc_queue_t *q, void *item);
bool mpmc_dequeue(mpmc_queue_t *q, void **out);
```

**Constraints:**
- Implement the Michael and Scott two-lock-free queue, then convert to fully lock-free using CAS on head and tail.
- Solve ABA with either tagged pointers (64-bit pointer plus 64-bit counter via double-width CAS) or hazard pointers.
- Discuss the memory reclamation problem explicitly.

**Follow-ups:**
- Why is the naive CAS-based queue subject to ABA?
- Compare hazard pointers, epoch-based reclamation, and RCU.

---

### Problem 10: IPC Mechanism Tradeoff Discussion (Whiteboard)
Not pure code. Sketch three implementations for passing 1 KB messages between two processes 100,000 times per second:

1. Unix pipe
2. POSIX shared memory plus a futex or semaphore
3. Mach port

Compare on latency, throughput, security, and complexity. Identify when each wins. Be ready to write the shared-memory ring buffer implementation if pushed.

---

## Low-Level Debugging Scenarios

These are talk-through problems, not coding problems. Be ready to narrate your reasoning.

### Scenario A
A daemon's RSS grows by 4 MB per hour but `leaks` reports nothing. Walk through your investigation.

### Scenario B
A multithreaded service crashes once per week with a nil deref at the same instruction. The stack involves a callback fired from libdispatch. Walk through your investigation.

### Scenario C
After a kernel update, a workload that previously ran 100 ms now takes 140 ms. CPU utilization is identical. Walk through your investigation.

### Scenario D
You receive a kernel panic log showing a deadlock between two `lck_mtx_t` acquisitions. The backtrace shows thread A holding lock X waiting on lock Y, and thread B holding lock Y waiting on lock X. Beyond the obvious lock ordering fix, what would you propose to prevent this class of bug going forward?
