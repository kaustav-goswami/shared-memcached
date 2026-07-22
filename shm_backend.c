/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 * shm_backend.c – shared-memory backend for memcached.
 *
 * Provides two entry points:
 *   shm_backend_create()  – called by process 1; creates and initialises the
 *                           region, slab arena, hash table, and all locks.
 *   shm_backend_attach()  – called by process 2+; attaches to an existing
 *                           region and waits for the creator to finish init.
 *
 * Address-space contract (important for gem5 / DAX):
 *   settings.shm_size is the TOTAL shared region size.  Every live object
 *   (shm_alloc metadata, slab arena, hashtable, control block) is placed
 *   inside [region_base, region_base + region_size).  The slab arena is
 *   carved from what remains after hashtable + control + a metadata reserve,
 *   so DAX-relative offsets never exceed the configured window.
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
#include <unistd.h>    /* sysconf */

/* Include memcached.h for compile-time constant checks only.
 * We forward-declare item in shm_backend.h so the struct body is not needed. */
#include "memcached.h"

/* Sanity checks: the constants in shm_backend.h must match memcached.h */
_Static_assert(SHM_MAX_SLAB_CLASSES == MAX_NUMBER_OF_SLAB_CLASSES,
               "SHM_MAX_SLAB_CLASSES does not match MAX_NUMBER_OF_SLAB_CLASSES");
_Static_assert(SHM_POWER_LARGEST == POWER_LARGEST,
               "SHM_POWER_LARGEST does not match POWER_LARGEST");

/* Bytes reserved inside the region for shm_alloc header/directory, per-object
 * block headers, alignment slack, and a free-list remainder.  Kept deliberately
 * generous so carving never overruns the region. */
#define SHM_META_RESERVE  (8ull * 1024 * 1024)

/* ── Internal helpers ────────────────────────────────────────────────────── */

static size_t page_size_bytes(void)
{
    long p = sysconf(_SC_PAGESIZE);
    return (p > 0) ? (size_t)p : 4096u;
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

    fprintf(stderr, "shm: ctrl=%p (no DAX-resident mutexes; plain LD/ST only)\n",
            (void *)ctrl);
}

/* Bytes consumed by hashtable + control block + metadata reserve + guards. */
static size_t region_fixed_overhead(uint32_t ht_power, uint32_t guard_pages)
{
    size_t ht_size   = ((size_t)1 << ht_power) * sizeof(void *);
    size_t ctrl_size = sizeof(shm_control_block_t);
    size_t guards    = (size_t)guard_pages * page_size_bytes();
    return ht_size + ctrl_size + (size_t)SHM_META_RESERVE + guards;
}

/*
 * Carve the slab arena out of the total region so that
 *   region_hdr + directory + block headers + slab + ht + ctrl + guards
 * all fit inside [0, region_size).
 *
 * Previously shm_size was treated as the arena size alone; the arena was then
 * placed after shm_alloc metadata, so item pointers near the end of a "512 MB"
 * arena had DAX-relative offsets > 512 MB and tripped gem5 bounds checks.
 */
static size_t slab_arena_from_region(size_t region_size, uint32_t ht_power,
                                     uint32_t guard_pages)
{
    size_t overhead = region_fixed_overhead(ht_power, guard_pages);
    if (region_size <= overhead)
        return 0;
    return region_size - overhead;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

int shm_backend_create(const char    *name,
                       size_t         region_size,
                       uint32_t       hashtable_power,
                       shm_backend_t  backend,
                       uint32_t       guard_pages,
                       mc_shm_backend_t **out)
{
    if (!name || region_size < 4096 || hashtable_power > 30 || !out)
        return EINVAL;
    if (backend != SHM_BACKEND_POSIX && backend != SHM_BACKEND_DAX)
        return EINVAL;

    size_t ht_size   = ((size_t)1 << hashtable_power) * sizeof(void *);
    size_t slab_size = slab_arena_from_region(region_size, hashtable_power,
                                              guard_pages);
    if (slab_size == 0) {
        fprintf(stderr,
                "shm_backend_create: region %zu bytes is too small for "
                "hashtable (%zu) + control block (%zu) + metadata reserve "
                "(%llu) + guard_pages (%u)\n",
                region_size, ht_size, sizeof(shm_control_block_t),
                (unsigned long long)SHM_META_RESERVE, guard_pages);
        return EINVAL;
    }

    mc_shm_backend_t *b = calloc(1, sizeof(*b));
    if (!b) return ENOMEM;

    /*
     * Map exactly `region_size` bytes for both backends.
     * POSIX: ftruncate to region_size.
     * DAX:   mmap the first region_size bytes of the device (must be ≤ capacity;
     *        enforced inside shm_region_open).  REQUIRE_FIXED so we never land
     *        near libc ASLR with a too-large VMA (gem5 PA 0x1839ADF20 class).
     *
     * Do NOT map the entire DAX device when the caller asked for a smaller
     * window — that would let allocations land past the gem5-visible bound.
     */
    shm_region_open_opts_t opts = {
        .backend      = backend,
        .flags        = SHM_OPEN_CREATE | SHM_OPEN_REQUIRE_FIXED,
        .dir_capacity = 16,
        .guard_pages  = guard_pages,
    };
    int rc = shm_region_open(name, region_size, &opts, &b->region);
    if (rc != 0) { free(b); return rc; }

    if (shm_region_size(b->region) < region_size) {
        fprintf(stderr,
                "shm_backend_create: mapped only %zu bytes, need %zu\n",
                shm_region_size(b->region), region_size);
        shm_region_close(b->region, backend == SHM_BACKEND_POSIX);
        free(b);
        return ENOSPC;
    }

    void *region_base = shm_region_base(b->region);
    b->region_base = region_base;

    /* Allocate the slab arena */
    uint64_t  slab_id;
    shm_off_t slab_off;
    rc = shm_alloc(b->region, slab_size, "slab_arena", 1, SHM_PERM_DEFAULT,
                   1, &slab_id, &slab_off);
    if (rc != 0) { shm_region_close(b->region, true); free(b); return rc; }

    b->slab_arena = shm_ptr(b->region, slab_off, 1, SHM_PERM_DEFAULT,
                            SHM_PERM_READ | SHM_PERM_WRITE);
    b->slab_size  = slab_size;
    b->region_size = region_size;

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
    if (!b->ctrl || !b->slab_arena || !b->ht_arena) {
        shm_region_close(b->region, true);
        free(b);
        return EFAULT;
    }

    /* Bounds check: every shared object must lie inside the region window. */
    size_t slab_end = (size_t)((char *)b->slab_arena - (char *)region_base)
                    + slab_size;
    size_t ht_end   = (size_t)((char *)b->ht_arena - (char *)region_base)
                    + ht_size;
    size_t ctrl_end = (size_t)((char *)b->ctrl - (char *)region_base)
                    + sizeof(shm_control_block_t);
    if (slab_end > region_size || ht_end > region_size || ctrl_end > region_size) {
        fprintf(stderr,
                "shm_backend_create: internal layout exceeds region_size %zu "
                "(slab_end=%zu ht_end=%zu ctrl_end=%zu)\n",
                region_size, slab_end, ht_end, ctrl_end);
        shm_region_close(b->region, true);
        free(b);
        return EFAULT;
    }

    fprintf(stderr,
            "shm: layout region=%zu MB  slab_arena=%zu MB "
            "(off=%zu..%zu)  hashtable=%zu KB  ctrl=%zu KB  VA=%p "
            "max_end=%zu (must be <= region) guard_pages=%u\n",
            region_size / (1024 * 1024),
            slab_size / (1024 * 1024),
            (size_t)((char *)b->slab_arena - (char *)region_base),
            slab_end,
            ht_size / 1024,
            sizeof(shm_control_block_t) / 1024,
            region_base,
            (slab_end > ht_end ? (slab_end > ctrl_end ? slab_end : ctrl_end)
                               : (ht_end > ctrl_end ? ht_end : ctrl_end)),
            guard_pages);

    /* Initialise the control block (mutexes, slab_list pointers, zero state) */
    init_ctrl(b->ctrl, b->slab_arena, slab_size, b->ht_arena, hashtable_power);

    b->is_creator = true;
    *out = b;
    return 0;
}

int shm_backend_attach(const char    *name,
                       shm_backend_t  backend,
                       size_t         region_size,
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
        .flags   = SHM_OPEN_REQUIRE_FIXED,
    };
    int rc = shm_region_open(name, region_size, &opts, &b->region);
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
    b->region_size     = shm_region_size(b->region);
    b->hashtable_power = b->ctrl->hashpower;
    b->is_creator      = false;
    b->region_base     = shm_region_base(b->region);

    /*
     * ctrl->mem_base / primary_hashtable were stored as absolute VAs by the
     * creator.  shm_ptr() returns addresses relative to *this* process's map.
     * They must match — otherwise ASLR gave us a different base and following
     * raw item/hashtable pointers produces wrong DAX offsets (gem5 OOB, e.g.
     * PA 0x1839ADF20 past a 2 GiB shm_size window).
     */
    {
        void *region_base = b->region_base;
        size_t rsz = b->region_size;
        size_t slab_off = (size_t)((char *)b->slab_arena - (char *)region_base);
        size_t ht_off   = (size_t)((char *)b->ht_arena - (char *)region_base);
        size_t ctrl_off = (size_t)((char *)b->ctrl - (char *)region_base);

        if (b->ctrl->mem_base != b->slab_arena
                || b->ctrl->primary_hashtable != b->ht_arena) {
            fprintf(stderr,
                    "shm_backend_attach: creator/attacher VA mismatch\n"
                    "  local base=%p  ctrl->mem_base=%p  local slab=%p\n"
                    "  ctrl->ht=%p  local ht=%p\n"
                    "  Re-create the region after rebuild; ensure attach maps "
                    "at the creator VA (see shm_alloc map_base_addr).\n",
                    region_base, b->ctrl->mem_base, b->slab_arena,
                    b->ctrl->primary_hashtable, b->ht_arena);
            shm_region_close(b->region, false);
            free(b);
            return EFAULT;
        }

        if (slab_off + b->slab_size > rsz || ht_off >= rsz || ctrl_off >= rsz) {
            fprintf(stderr,
                    "shm_backend_attach: object past region_size %zu "
                    "(slab_off=%zu ht_off=%zu ctrl_off=%zu)\n",
                    rsz, slab_off, ht_off, ctrl_off);
            shm_region_close(b->region, false);
            free(b);
            return EFAULT;
        }

        fprintf(stderr,
                "shm: attach ok region=%zu MB VA=%p slab_off=%zu ht_off=%zu "
                "ctrl_off=%zu\n",
                rsz / (1024 * 1024), region_base, slab_off, ht_off, ctrl_off);
    }

    *out = b;
    return 0;
}

void shm_backend_destroy(mc_shm_backend_t *b, bool unlink)
{
    if (!b) return;
    shm_region_close(b->region, unlink);
    free(b);
}
