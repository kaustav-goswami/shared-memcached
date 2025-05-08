# README FOR DISAGGREGATED MEMORY

Written by Kaustav Goswami. Reach out to me at kggoswami@ucdavis.edu

This document explains how to use /dev/dax for memcached for shared
disaggregated memories.

## Building

```sh
# For memcached
git clone https://github.com/kaustav-goswami/memcached.git
cd memcached
git checkout disaggregated
./autogen.sh
./configure
# May fail to compile with undefined reference to shm_open
make -j32
# If it fails, then export LIBS=$LIBS -lrt or update the Makefile with
# LIBS = -levent -lrt and run make again
cd ..
```

## User agnostic changes

The changes are mostly added in `slabs.c` where the mmap call is modified to
use dmalloc() to map into a /dev/dax region.
It is forced to allocate the memory in `/dev/dax`.
The amount of memory it requests is memcached's memory (`limit`/1 GiB) + 1
GiBs.
It is expected that the user running gem5 or SST will be responsible for
setting this up.

## Limitations

`make test` doesn't work on the repository due to white spaces in `dmalloc.h`.
I was too lazy to fix it but I have tested `make test` with a different
dmalloc.h with no stray white-spaces and it passed all the tests.
If you want to do the same, use Linux shared memory and get rid of the other
functions.

The data might not be updated at the remote memory due to caching.

