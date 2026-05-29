/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 * slabclass_t definition shared between slabs.c and shm_backend.h.
 * Kept in a separate header to avoid circular include dependencies.
 */
#ifndef SLABS_TYPES_H
#define SLABS_TYPES_H

#include <stdint.h>

/* Maximum number of slab pages per class when using the shm backend.
 * Pre-allocated inline in the control block; dynamic realloc is not used
 * in shared-memory mode. */
#define SLABS_SHM_MAX_LIST 1024

typedef struct {
    uint32_t     size;       /* chunk size in bytes */
    uint32_t     perslab;    /* number of chunks per slab page */
    void        *slots;      /* LIFO free-list of available chunks */
    unsigned int sl_curr;    /* number of free chunks on the list */
    unsigned int slabs;      /* number of slab pages allocated for this class */
    void       **slab_list;  /* array of slab page pointers (malloc or shm-backed) */
    unsigned int list_size;  /* capacity of slab_list */
} slabclass_t;

#endif /* SLABS_TYPES_H */
