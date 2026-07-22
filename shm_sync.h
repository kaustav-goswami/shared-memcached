/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 * shm_sync.h – lock stubs for the shared-memory / DAX backend.
 *
 * When g_shm_backend is active, memcached assumes the platform (gem5 with
 * hardware coherence) provides consistency without pthread mutex atomics on
 * DAX-resident words.  All SHM-side lock sites become no-ops.
 */
#ifndef SHM_SYNC_H
#define SHM_SYNC_H

#include <pthread.h>
#include <stdbool.h>

/*
 * Requires memcached.h (g_shm_backend) to be included before this header.
 */

static inline bool shm_sync_disabled(void)
{
    return g_shm_backend != NULL;
}

static inline void shm_sync_lock(pthread_mutex_t *m)
{
    if (!shm_sync_disabled())
        pthread_mutex_lock(m);
}

static inline void shm_sync_unlock(pthread_mutex_t *m)
{
    if (!shm_sync_disabled())
        pthread_mutex_unlock(m);
}

static inline int shm_sync_trylock(pthread_mutex_t *m)
{
    if (shm_sync_disabled())
        return 0;
    return pthread_mutex_trylock(m);
}

#endif /* SHM_SYNC_H */
