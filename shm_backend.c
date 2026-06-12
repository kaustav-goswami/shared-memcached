/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 * shm_backend.c – shared-memory backend for memcached.
 *
 * Provides two entry points:
 *   shm_backend_create()  – called by process 1; creates and initialises the
 *                           region, slab arena, hash table, and all locks.
 *   shm_backend_attach()  – called by process 2+; attaches to an existing
 *                           region and waits for the creator to finish init.
 */

#define _POSIX_C_SOURCE 200809L

#include "shm_backend.h"

#include <assert.h>
#include <errno.h>
#include <sched.h>     /* sched_yield */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>      /* nanosleep */

/* Include memcached.h for compile-time constant checks only.
 * We forward-declare item in shm_backend.h so the struct body is not needed. */
#include "memcached.h"

/* Sanity checks: the constants in shm_backend.h must match memcached.h */
_Static_assert(SHM_MAX_SLAB_CLASSES == MAX_NUMBER_OF_SLAB_CLASSES,
               "SHM_MAX_SLAB_CLASSES does not match MAX_NUMBER_OF_SLAB_CLASSES");
_Static_assert(SHM_POWER_LARGEST == POWER_LARGEST,
               "SHM_POWER_LARGEST does not match POWER_LARGEST");

/* ── Internal helpers ────────────────────────────────────────────────────── */

static void init_pshared_mutex(pthread_mutex_t *m)
{
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
    pthread_mutex_init(m, &attr);
    pthread_mutexattr_destroy(&attr);
}

static void init_ctrl(shm_control_block_t *ctrl,
                      void                *slab_arena,
                      size_t               slab_size,
                      void                *ht_arena,
                      uint32_t             ht_power)
{
    int i;

    memset(ctrl, 0, sizeof(*ctrl));

    ctrl->initialized         = 0;   /* set to 1 after full init */
    ctrl->power_largest       = 0;   /* set by slabs_init() */
    ctrl->mem_base            = slab_arena;
    ctrl->mem_current         = slab_arena;
    ctrl->mem_avail           = slab_size;
    ctrl->mem_limit           = slab_size;
    ctrl->mem_malloced        = 0;
    ctrl->mem_limit_reached   = 0;
    ctrl->primary_hashtable   = ht_arena;
    ctrl->hashpower           = ht_power;
    ctrl->item_lock_hashpower = SHM_ITEM_LOCK_HASHPOWER;
    ctrl->item_lock_count     = SHM_ITEM_LOCK_COUNT;
    ctrl->cas_id              = 1;

    /* Wire up each slabclass.slab_list to its inline backing array.
     * list_size is fixed at SLABS_SHM_MAX_LIST; no dynamic realloc. */
    for (i = 0; i < SHM_MAX_SLAB_CLASSES; i++) {
        ctrl->slabclass[i].slab_list  = ctrl->sc_slab_list[i];
        ctrl->slabclass[i].list_size  = SLABS_SHM_MAX_LIST;
    }

    /* Initialise all process-shared mutexes */
    init_pshared_mutex(&ctrl->slabs_lock);
    init_pshared_mutex(&ctrl->cas_id_lock);
    for (i = 0; i < SHM_ITEM_LOCK_COUNT; i++)
        init_pshared_mutex(&ctrl->item_locks[i]);
    for (i = 0; i < SHM_POWER_LARGEST; i++)
        init_pshared_mutex(&ctrl->lru_locks[i]);
}

/* Compute total region size given slab and hash-table requirements. */
static size_t region_total_size(size_t slab_size, uint32_t ht_power)
{
    size_t ht_size   = ((size_t)1 << ht_power) * sizeof(void *);
    size_t ctrl_size = sizeof(shm_control_block_t);
    /* 8 MB overhead for shm_alloc metadata + directory + alignment slack */
    size_t overhead  = 8 * 1024 * 1024;
    size_t total     = ctrl_size + slab_size + ht_size + overhead;
    /* Round up to 2 MB boundary */
    return (total + (2 * 1024 * 1024 - 1)) & ~(size_t)(2 * 1024 * 1024 - 1);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

int shm_backend_create(const char    *name,
                       size_t         slab_size,
                       uint32_t       hashtable_power,
                       shm_backend_t  backend,
                       mc_shm_backend_t **out)
{
    if (!name || slab_size == 0 || hashtable_power > 30 || !out)
        return EINVAL;
    if (backend != SHM_BACKEND_POSIX && backend != SHM_BACKEND_DAX)
        return EINVAL;

    mc_shm_backend_t *b = calloc(1, sizeof(*b));
    if (!b) return ENOMEM;

    size_t total = region_total_size(slab_size, hashtable_power);
    size_t ht_size = ((size_t)1 << hashtable_power) * sizeof(void *);

    /* Create the shm_alloc region */
    shm_region_open_opts_t opts = {
        .backend      = backend,
        .flags        = SHM_OPEN_CREATE,
        .dir_capacity = 16,
    };
    /*
     * POSIX: ftruncate to `total` and map that many bytes.
     * DAX: map the entire device (size=0 → fstat); verify it fits `total`.
     */
    size_t open_size = (backend == SHM_BACKEND_DAX) ? 0 : total;
    int rc = shm_region_open(name, open_size, &opts, &b->region);
    if (rc != 0) { free(b); return rc; }

    if (backend == SHM_BACKEND_DAX) {
        size_t dev_size = shm_region_size(b->region);
        if (total > dev_size) {
            fprintf(stderr,
                    "shm_backend_create: DAX device '%s' is %zu bytes but "
                    "%zu bytes are required (slab arena %zu + metadata)\n",
                    name, dev_size, total, slab_size);
            shm_region_close(b->region, false);
            free(b);
            return ENOSPC;
        }
    }

    /* Allocate the slab arena */
    uint64_t  slab_id;
    shm_off_t slab_off;
    rc = shm_alloc(b->region, slab_size, "slab_arena", 1, SHM_PERM_DEFAULT,
                   1, &slab_id, &slab_off);
    if (rc != 0) { shm_region_close(b->region, true); free(b); return rc; }

    b->slab_arena = shm_ptr(b->region, slab_off, 1, SHM_PERM_DEFAULT,
                            SHM_PERM_READ | SHM_PERM_WRITE);
    b->slab_size  = slab_size;

    /* Allocate the hash table bucket array (zeroed by shm_alloc) */
    uint64_t  ht_id;
    shm_off_t ht_off;
    rc = shm_alloc(b->region, ht_size, "hashtable", 1, SHM_PERM_DEFAULT,
                   2, &ht_id, &ht_off);
    if (rc != 0) { shm_region_close(b->region, true); free(b); return rc; }

    b->ht_arena        = shm_ptr(b->region, ht_off, 1, SHM_PERM_DEFAULT,
                                 SHM_PERM_READ | SHM_PERM_WRITE);
    b->hashtable_power = hashtable_power;

    /* Allocate the control block */
    uint64_t  ctrl_id;
    shm_off_t ctrl_off;
    rc = shm_alloc(b->region, sizeof(shm_control_block_t), "shm_ctrl",
                   1, SHM_PERM_DEFAULT, 3, &ctrl_id, &ctrl_off);
    if (rc != 0) { shm_region_close(b->region, true); free(b); return rc; }

    b->ctrl = (shm_control_block_t *)shm_ptr(b->region, ctrl_off, 1,
                                             SHM_PERM_DEFAULT,
                                             SHM_PERM_READ | SHM_PERM_WRITE);
    if (!b->ctrl) {
        shm_region_close(b->region, true);
        free(b);
        return EFAULT;
    }

    /* Initialise the control block (mutexes, slab_list pointers, zero state) */
    init_ctrl(b->ctrl, b->slab_arena, slab_size, b->ht_arena, hashtable_power);

    b->is_creator = true;
    *out = b;
    return 0;
}

int shm_backend_attach(const char    *name,
                       shm_backend_t  backend,
                       mc_shm_backend_t **out)
{
    if (!name || !out)
        return EINVAL;
    if (backend != SHM_BACKEND_POSIX && backend != SHM_BACKEND_DAX)
        return EINVAL;

    mc_shm_backend_t *b = calloc(1, sizeof(*b));
    if (!b) return ENOMEM;

    shm_region_open_opts_t opts = {
        .backend = backend,
        .flags   = 0,
    };
    int rc = shm_region_open(name, 0, &opts, &b->region);
    if (rc != 0) { free(b); return rc; }

    /* Locate the control block by name */
    uint64_t  ctrl_id;
    shm_off_t ctrl_off;
    rc = shm_lookup_by_name(b->region, "shm_ctrl", 1, SHM_PERM_ADMIN,
                            &ctrl_id, &ctrl_off);
    if (rc != 0) {
        shm_region_close(b->region, false);
        free(b);
        return rc;
    }
    b->ctrl = (shm_control_block_t *)shm_ptr(b->region, ctrl_off, 1,
                                             SHM_PERM_ADMIN,
                                             SHM_PERM_READ | SHM_PERM_WRITE);

    /* Locate the slab arena */
    uint64_t  slab_id;
    shm_off_t slab_off;
    rc = shm_lookup_by_name(b->region, "slab_arena", 1, SHM_PERM_ADMIN,
                            &slab_id, &slab_off);
    if (rc != 0) {
        shm_region_close(b->region, false);
        free(b);
        return rc;
    }
    b->slab_arena = shm_ptr(b->region, slab_off, 1, SHM_PERM_ADMIN,
                            SHM_PERM_READ | SHM_PERM_WRITE);

    /* Locate the hash table */
    uint64_t  ht_id;
    shm_off_t ht_off;
    rc = shm_lookup_by_name(b->region, "hashtable", 1, SHM_PERM_ADMIN,
                            &ht_id, &ht_off);
    if (rc != 0) {
        shm_region_close(b->region, false);
        free(b);
        return rc;
    }
    b->ht_arena = shm_ptr(b->region, ht_off, 1, SHM_PERM_ADMIN,
                          SHM_PERM_READ | SHM_PERM_WRITE);

    /* Wait until the creator finishes initialisation */
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000 }; /* 1 ms */
    int waited = 0;
    while (!b->ctrl->initialized) {
        nanosleep(&ts, NULL);
        if (++waited > 30000) { /* 30 s timeout */
            fprintf(stderr, "shm_backend_attach: timed out waiting for creator\n");
            shm_region_close(b->region, false);
            free(b);
            return ETIMEDOUT;
        }
    }

    /* Fill in remaining fields from the now-ready control block */
    b->slab_size       = b->ctrl->mem_limit;
    b->hashtable_power = b->ctrl->hashpower;
    b->is_creator      = false;

    *out = b;
    return 0;
}

void shm_backend_destroy(mc_shm_backend_t *b, bool unlink)
{
    if (!b) return;
    shm_region_close(b->region, unlink);
    free(b);
}
