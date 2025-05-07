#ifndef DMALLOC_H_
#define DMALLOC_H_

// This file is taken from simple-graph to allocate memory for the graph
// software. It understands master and worker to make sure that the clients are
// not allowed to modify the graph in the remote host.
//
// For simple-graph,
// see https://github.com/kaustav-goswami/disaggregated-shared-graphs

// The master node should ONLY be responsible for allocating the graph.

// For testing on a local host, the code can switch to use a hugepage. Huge
// pages must be enabled to do so.

// For local testing without sude, shmem was added too. By default, the shmem
// file is opened in the gapbs version.

// We need a overlord function that manages all the different objects for
// allocations. pvector is called multiple times, which makes managing a flat
// range of memory difficult.

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <assert.h>
#include <time.h>

#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>

#define ONE_G 0x40000000

// This is not bad this is dangerous
#define nullptr NULL

// This file has three functions to mount  SHARED_MEM segment. For true
// disaggregated memory, you need to use dmalloc(size_t,int);
int* dmalloc(size_t, int);
// For quick testing on a single host, use shmalloc(size_t,int) which uses
// linux shmem
int* shmalloc(size_t, int);

// a utility function clflushopt is needed to flush the cache of the write
// nodes
int* dmalloc(size_t size, int host_id) {
    // hehe, I love the name
    //
    // This function will create a MAP_SHARED memory map for the graph. The
    // idea is to allocate the graph (potentially a large graph) in the
    // remote/disaggregated memory and only the master will have RDWR flag. All
    // other hosts should only have READ permission.
    //
    // @params
    // :size: Size of requested memory in GiB
    // :host_id: An ID sent to dictate whether this is a master or a worker
    //
    // @returns
    // A pointer to the mmap call
    //
    int fd;
    // It is assumed that we are working with DAX devices. Will change change
    // to something else if needed later.
    const char *path = "/dev/dax0.0";
    
    // check if the caller is the master node
    if (host_id == 0)
        fd = open(path, O_RDWR);
    else
        // these are all graph workers. Prevent these workers from writing into
        // the graph.
        fd = open(path, O_RDONLY);

    // make sure that the open call was successful. Otherwise notify the user
    // before exiting.
    if (fd < 0) {
        printf("Error mounting! Make sure that the mount point %s is valid\n",
                path);
        exit(EXIT_FAILURE);
    }
    // Try allocating the required size for the graph. This might be
    // complicated if the graph is very large!
    // With all the testings, it seems that the mmap call is not a problem.
    // Please submit a bug report/issue if you see an error.

    // Make sure only the allocator has READ/WRITE permission. This forces
    // software coherence!
    int *ptr;
    if (host_id == 0) {
        ptr = (int *) mmap (nullptr, size * ONE_G,
                                    PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    }
    else {
        ptr = (int *) mmap (nullptr, size * ONE_G,
                                            PROT_READ, MAP_SHARED, fd, 0);

    }

    // The map may fail due to several reasons but we notify the user.
    if (ptr == MAP_FAILED) {
        printf("The mmap call failed! Maybe it's too huge?\n");
        exit(EXIT_FAILURE);
    }
    // The mmap was successful! return the pointer to the user.
    return ptr;
}

int* shmalloc(size_t size, int host_id) {
    // To be used with LINUX SHMEM on a single host for testing only
    //
    // This function will create a MAP_SHARED memory map for the graph. The
    // idea is to allocate the graph (potentially a large graph) in the
    // remote/disaggregated memory and only the master will have RDWR flag. All
    // other hosts should only have READ permission.
    //
    // @params
    // :size: Size of requested memory in GIB
    // :host_id: An ID sent to dictate whether this is a master or a worker
    //
    // @returns
    // A pointer to the mmap call
    //
    // XXX: WARNING, COHERENCE IS NOT ENFORCED when opening the file!
    int fd = shm_open("/my_shmem2", O_CREAT | O_RDWR | O_LARGEFILE, 0666);
    if (fd == -1) {
        perror("shm_open");
        exit(EXIT_FAILURE);
    }

    if (ftruncate(fd, size * ONE_G) == -1) {
        perror("ftruncate");
        exit(EXIT_FAILURE);
    }

    int *ptr;
    // PROT_READ/WRITE is enforced here.
    // depending upon the host id, we'll set the read/write permissons.
    if (host_id == 0) {
        ptr = (int *) mmap(NULL, size * ONE_G,
                                    PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    }
    else {
        // This is a client host.
        ptr = (int *) mmap(NULL, size * ONE_G, PROT_READ, MAP_SHARED, fd, 0);
    }
    // The map may fail due to several reasons but we notify the user.
    if (ptr == MAP_FAILED) {
        printf("The mmap call failed! Maybe it's too huge?\n");
        exit(EXIT_FAILURE);
    }
    return ptr;
}
#endif // DMALLOC_H
