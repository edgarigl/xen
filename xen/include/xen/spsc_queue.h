// Single Producer Single Consumer Queue implemented over shared-memory

// Copyright (c) 2023 Zero ASIC Corporation
// This code is licensed under Apache License 2.0 (see LICENSE for details)

#ifndef SPSC_QUEUE_H__
#define SPSC_QUEUE_H__

#include <xen/ctype.h>
#define assert ASSERT

#define SPSC_QUEUE_MAX_PACKET_SIZE 64
#define SPSC_QUEUE_CACHE_LINE_SIZE 64

typedef struct spsc_queue_shared {
    int32_t head __attribute__((__aligned__(SPSC_QUEUE_CACHE_LINE_SIZE)));
    int32_t tail __attribute__((__aligned__(SPSC_QUEUE_CACHE_LINE_SIZE)));
    uint32_t packets[1][SPSC_QUEUE_MAX_PACKET_SIZE / 4]
        __attribute__((__aligned__(SPSC_QUEUE_CACHE_LINE_SIZE)));
} spsc_queue_shared;

typedef struct spsc_queue {
    int32_t cached_tail __attribute__((__aligned__(SPSC_QUEUE_CACHE_LINE_SIZE)));
    int32_t cached_head __attribute__((__aligned__(SPSC_QUEUE_CACHE_LINE_SIZE)));
    spsc_queue_shared* shm;
    char* name;
    int capacity;

    bool unmap_at_close;
} spsc_queue;

// Returns the capacity of a queue given a specific mapsize.
static inline int spsc_capacity(size_t mapsize) {
    spsc_queue* q = NULL;
    int capacity;

    if (mapsize < sizeof(*q->shm)) {
        return 0;
    }

    // Start with the size of the shared area. This includes the
    // control members + one packet.
    mapsize -= sizeof(*q->shm);

    capacity = mapsize / sizeof(q->shm->packets[0]) + 1;

    if (capacity < 2) {
        // Capacities less than 2 are invalid.
        return 0;
    }

    return capacity;
}

static inline size_t spsc_mapsize(int capacity) {
    spsc_queue* q = NULL;
    size_t mapsize;

    assert(capacity >= 2);

    // Start with the size of the shared area. This includes the
    // control members + one packet.
    mapsize = sizeof(*q->shm);
    // Add additional packets.
    mapsize += sizeof(q->shm->packets[0]) * (capacity - 1);

    return mapsize;
}

static inline spsc_queue* spsc_open_mem(const char* name, size_t capacity, void* mem) {
    spsc_queue* q = NULL;
    size_t mapsize;
    void* p;

    // Compute the size of the SHM mapping.
    mapsize = spsc_mapsize(capacity);

    // Allocate spsc-queue.
    p = xmalloc(spsc_queue);
    if (!p) {
        goto err;
    }
    q = (spsc_queue*) p;
    memset(q, 0, sizeof *q);

    p = mem;
    assert(mem);

    q->shm = (spsc_queue_shared*) p;
    q->name = NULL;
    q->capacity = capacity;

    /* In case we're opening a pre-existing queue, pick up where we left off. */
    __atomic_load(&q->shm->tail, &q->cached_tail, __ATOMIC_RELAXED);
    __atomic_load(&q->shm->head, &q->cached_head, __ATOMIC_RELAXED);
    return q;

err:
    xfree(q);
    return NULL;
}

static inline void spsc_close(spsc_queue* q) {
    size_t mapsize;

    if (!q) {
        return;
    }

    mapsize = spsc_mapsize(q->capacity);

    xfree(q);
}

static inline int spsc_size(spsc_queue* q) {
    int head, tail;
    int size;

    __atomic_load(&q->shm->head, &head, __ATOMIC_ACQUIRE);
    __atomic_load(&q->shm->tail, &tail, __ATOMIC_ACQUIRE);

    size = head - tail;
    if (size < 0) {
        size += q->capacity;
    }
    return size;
}

static inline bool spsc_send(spsc_queue* q, void* buf, size_t size) {
    int next_head;
    // get pointer to head
    int head;

    __atomic_load(&q->shm->head, &head, __ATOMIC_RELAXED);

    assert(size <= sizeof q->shm->packets[0]);

    // compute the head pointer
    next_head = head + 1;
    if (next_head == q->capacity) {
        next_head = 0;
    }

    // if the queue is full, bail out
    if (next_head == q->cached_tail) {
        __atomic_load(&q->shm->tail, &q->cached_tail, __ATOMIC_ACQUIRE);
        if (next_head == q->cached_tail) {
            return false;
        }
    }

    // otherwise write in the packet
    memcpy(q->shm->packets[head], buf, size);

    // and update the head pointer
    __atomic_store(&q->shm->head, &next_head, __ATOMIC_RELEASE);

    return true;
}

static inline bool spsc_recv_base(spsc_queue* q, void* buf, size_t size, bool pop) {
    // get the read pointer
    int tail;
    __atomic_load(&q->shm->tail, &tail, __ATOMIC_RELAXED);

    assert(size <= sizeof q->shm->packets[0]);

    // if the queue is empty, bail out
    if (tail == q->cached_head) {
        __atomic_load(&q->shm->head, &q->cached_head, __ATOMIC_ACQUIRE);
        if (tail == q->cached_head) {
            return false;
        }
    }

    // otherwise read out the packet
    memcpy(buf, q->shm->packets[tail], size);

    if (pop) {
        // and update the read pointer
        tail++;
        if (tail == q->capacity) {
            tail = 0;
        }
        __atomic_store(&q->shm->tail, &tail, __ATOMIC_RELEASE);
    }

    return true;
}

static inline bool spsc_recv(spsc_queue* q, void* buf, size_t size) {
    return spsc_recv_base(q, buf, size, true);
}

static inline bool spsc_recv_peek(spsc_queue* q, void* buf, size_t size) {
    return spsc_recv_base(q, buf, size, false);
}
#endif // _SPSC_QUEUE
