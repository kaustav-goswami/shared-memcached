/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 * shm_backend.h – shared-memory backend for memcached.
 *
 * Wraps the shm_alloc library to provide a single POSIX shared-memory region
 * that holds all mutable cache state:
 *
 *   - Slab arena   (raw slab pages; items, keys, and values live here)
 *   - Hash table   (primary_hashtable bucket array)
 *   - Control block (slab-class metadata, LRU heads/tails, all locks)
 *
 * Two processes can attach to the same region and serve different TCP ports
 * while sharing the same live key-value data.  The region is mapped at the
 * same virtual address in every attaching process (using MAP_FIXED_NOREPLACE
 * on the creator's mmap base, stored in the region header) so that raw C
 * pointers embedded in slab data (item->next, item->h_next, etc.) remain
 * valid across processes without offset translation.
 */
#ifndef SHM_BACKEND_H
#define SHM_BACKEND_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

#include "allocator/include/shm_alloc.h"
#include "slabs_types.h"   /* slabclass_t, SLABS_SHM_MAX_LIST */

/* ── Compile-time constants ─────────────────────────────────────────────── */

/* Must match MAX_NUMBER_OF_SLAB_CLASSES in memcached.h */
#define SHM_MAX_SLAB_CLASSES    64

/* Must match POWER_LARGEST in memcached.h */
#define SHM_POWER_LARGEST       256

/* Hash power for the item lock table in shm mode.
 * Fixed at 13 (8192 buckets) so both processes use the same layout. */
#define SHM_ITEM_LOCK_HASHPOWER 13
#define SHM_ITEM_LOCK_COUNT     (1u << SHM_ITEM_LOCK_HASHPOWER)

/* ── Shared control block ───────────────────────────────────────────────── */

/*
 * Lives in the shm_alloc region (named "shm_ctrl").  All mutable slab and
 * LRU state that must be visible to every attached process is stored here.
 *
 * All pointer fields are void* to avoid depending on memcached.h types
 * (prevents circular includes).  Callers cast them to the correct type.
 *
 * Layout note: the lock arrays are placed at the end (after the smaller
 * scalar fields) so that the structure compiles cleanly at any padding.
 */
typedef struct {
    /* ── Lifecycle ──────────────────────────────────────────────────── */
    volatile int32_t  initialized;    /* set to 1 by creator after full init */
    int32_t           power_largest;  /* highest active slab class id */

    /* ── Slab arena bookkeeping ──────────────────────────────────────── */
    void             *mem_base;       /* start of the slab arena allocation */
    void             *mem_current;    /* bump pointer into the arena          */
    size_t            mem_avail;      /* bytes remaining beyond mem_current   */
    size_t            mem_limit;      /* total arena size                     */
    size_t            mem_malloced;   /* bytes handed out so far              */
    int32_t           mem_limit_reached;
    int32_t           _pad1;

    /* ── Hash table ───────────────────────────────────────────────────── */
    void             *primary_hashtable; /* item** — points into ht region   */
    unsigned int      hashpower;         /* hash table size = 1 << hashpower  */

    /* ── Item lock table ─────────────────────────────────────────────── */
    unsigned int      item_lock_hashpower; /* always SHM_ITEM_LOCK_HASHPOWER */
    unsigned int      item_lock_count;     /* always SHM_ITEM_LOCK_COUNT     */

    /* ── CAS id ──────────────────────────────────────────────────────── */
    uint64_t          cas_id;

    /* ── LRU per-class state (void* for item* fields) ────────────────── */
    void             *lru_heads[SHM_POWER_LARGEST];
    void             *lru_tails[SHM_POWER_LARGEST];
    unsigned int      lru_sizes[SHM_POWER_LARGEST];
    uint64_t          lru_sizes_bytes[SHM_POWER_LARGEST];

    /* ── Slab class state ────────────────────────────────────────────── */
    /* slabclass_t structs; slab_list pointers point into sc_slab_list[][] */
    slabclass_t       slabclass[SHM_MAX_SLAB_CLASSES];

    /* Storage backing each slabclass[i].slab_list pointer (pre-allocated) */
    void             *sc_slab_list[SHM_MAX_SLAB_CLASSES][SLABS_SHM_MAX_LIST];

    /* ── Process-shared locks (placed last; large arrays) ───────────── */
    pthread_mutex_t   slabs_lock;
    pthread_mutex_t   cas_id_lock;
    pthread_mutex_t   item_locks[SHM_ITEM_LOCK_COUNT];
    pthread_mutex_t   lru_locks[SHM_POWER_LARGEST];
} shm_control_block_t;

/* ── Per-process backend handle ─────────────────────────────────────────── */

/*
 * Named mc_shm_backend_t to avoid a tag clash with the allocator library's
 * own  typedef enum shm_backend { ... } shm_backend_t;  definition.
 */
typedef struct mc_shm_backend {
    shm_region_t        *region;      /* shm_alloc region handle               */
    shm_control_block_t *ctrl;        /* pointer into shared control block      */
    void                *slab_arena;  /* pointer to start of slab arena         */
    size_t               slab_size;   /* total slab arena bytes                 */
    void                *ht_arena;    /* pointer to hash-table bucket array     */
    uint32_t             hashtable_power;
    bool                 is_creator;  /* true if this process created the region */
} mc_shm_backend_t;

/* ── API ─────────────────────────────────────────────────────────────────── */

/**
 * Create a fresh shared-memory region and initialise all shared state.
 *
 * @param name            POSIX shm name (e.g. "/memcached_shm") or DAX device
 *                        path (e.g. "/dev/dax0.0").
 * @param slab_size       Bytes to reserve for the slab arena.
 * @param hashtable_power Hash-table size = 1 << hashtable_power.
 * @param backend         SHM_BACKEND_POSIX or SHM_BACKEND_DAX.
 * @param out             Receives the backend handle on success.
 * @return 0 on success; errno-compatible code on failure.
 */
int shm_backend_create(const char       *name,
                       size_t            slab_size,
                       uint32_t          hashtable_power,
                       shm_backend_t     backend,
                       mc_shm_backend_t **out);

/**
 * Attach to an existing shared-memory region created by another process.
 * Blocks until the creator sets ctrl->initialized = 1.
 *
 * @param name    POSIX shm name or DAX device path used by the creator.
 * @param backend SHM_BACKEND_POSIX or SHM_BACKEND_DAX (must match creator).
 * @param out     Receives the backend handle on success.
 * @return 0 on success; errno-compatible code on failure.
 */
int shm_backend_attach(const char       *name,
                       shm_backend_t     backend,
                       mc_shm_backend_t **out);

/**
 * Detach and optionally destroy the shared-memory region.
 *
 * @param b      Backend handle (may be NULL).
 * @param unlink If true, shm_unlink() the POSIX shm name (only creator should).
 */
void shm_backend_destroy(mc_shm_backend_t *b, bool unlink);

#endif /* SHM_BACKEND_H */
