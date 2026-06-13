# Shared-memory backend for memcached

This document describes how memcached was modified to store **keys and values** in a single shared region backed by the `allocator/` **shm_alloc** library. The design targets benchmark evaluation on POSIX shared memory and **disaggregated / DAX / CXL** memory.

Two storage backends are supported (selected with `-o shm_backend=…`):

| Backend | `-o shm_backend=` | `-o shm_name=` value | Notes |
|---------|-------------------|----------------------|-------|
| **POSIX** (default) | `posix` | `/memcached_shm` | `shm_open` + `/dev/shm` |
| **DAX** | `dax` | `/dev/dax0.0` | Device node; fixed size, no `ftruncate` |

Two processes attach to the same region:

| Role | TCP port (default) | CLI flag | Purpose |
|------|-------------------|----------|---------|
| **Creator / loader** | `11211` | `-o shm_create` | Creates the SHM region, runs slab preallocation, loads the dataset (e.g. YCSB load phase) |
| **Attacher / worker** | `11212` | `-o shm_attach` | Maps the existing region, serves YCSB workload traffic against the same live data |

Both processes share the same slab arena, hash table, LRU lists, CAS counter, and mutexes.

---

## Quick start (YCSB)

Build:

```bash
echo 'm4_define([VERSION_NUMBER], [1.6.x-shm])' > version.m4   # once, if missing
autoreconf -fi && ./configure && make -j$(nproc)
```

### POSIX shared memory (default)

**Terminal 1 — creator on port 11211 (load phase):**

```bash
./memcached -p 11211 -U 0 -m 4096 \
  -o shm_name=/memcached_shm,shm_size=4096,shm_create,hashpower=20
# Run YCSB load against 127.0.0.1:11211
```

**Terminal 2 — attacher on port 11212 (workload phase):**

```bash
./memcached -p 11212 -U 0 -m 4096 \
  -o shm_name=/memcached_shm,shm_attach
# Run YCSB workload against 127.0.0.1:11212
```

### DAX / disaggregated memory

Requires a configured DAX device (e.g. `/dev/dax0.0`) large enough for the slab arena plus metadata (~8 MiB + hash table). The device must be **zeroed or freshly formatted** before the first `shm_create` — the creator initialises the `shm_alloc` region header on the device.

**Terminal 1 — creator on port 11211:**

```bash
./memcached -p 11211 -U 0 -m 4096 \
  -o shm_backend=dax,shm_name=/dev/dax0.0,shm_size=4096,shm_create,hashpower=20
```

**Terminal 2 — attacher on port 11212:**

```bash
./memcached -p 11212 -U 0 -m 4096 \
  -o shm_backend=dax,shm_name=/dev/dax0.0,shm_attach
```

Both processes must pass the **same** `shm_backend` and `shm_name` (device path). DAX devices cannot be `shm_unlink`’d; re-create requires clearing the device or using a fresh region.

Notes:

- `shm_name` is the region identifier:
  - **POSIX:** must start with `/` (e.g. `/memcached_shm`) — `shm_open` requirement.
  - **DAX:** device path (e.g. `/dev/dax0.0`) — passed to `open(O_RDWR)`.
- `shm_backend` is `posix` (default) or `dax`.
- `shm_size` is in **megabytes** (parsed by `-o shm_size=N`).
- `-m` (`maxbytes`) should match `shm_size` so accounting is consistent.
- Start the creator first; the attacher blocks until `ctrl->initialized == 1`.
- Stop the attacher before unlinking the region; only the creator should pass `unlink=true` on shutdown (not yet wired to a CLI flag — remove `/dev/shm/memcached_shm` manually if needed).

---

## Architecture

```
┌─────────────────────────────────────────────────────────────────────────┐
│  POSIX shm region  (shm_alloc)  name = e.g. "/memcached_shm"           │
│                                                                         │
│  ┌──────────────┐  ┌────────────────┐  ┌─────────────────────────────┐ │
│  │ region hdr   │  │ object dir     │  │ heap                         │ │
│  │ (shm_alloc)  │  │ slab_arena     │  │ hashtable                    │ │
│  │ map_base_addr│  │ hashtable      │  │ shm_ctrl (control block)     │ │
│  └──────────────┘  │ shm_ctrl       │  └─────────────────────────────┘ │
│                    └────────────────┘                                   │
│                                                                         │
│  slab_arena: bump-allocated slab pages (items = key + value in RAM)    │
│  hashtable:  item** buckets, size 2^hashpower                          │
│  shm_ctrl:   slabclass[], LRU heads/tails, locks, mem_* bookkeeping    │
└─────────────────────────────────────────────────────────────────────────┘
         ▲                                    ▲
         │ MAP_SHARED (+ MAP_FIXED_NOREPLACE) │
    Process 1 :11211                     Process 2 :11212
    shm_create                           shm_attach
```

**Pointer model:** Item structures in the slab arena hold raw C pointers (`item->next`, `item->h_next`, …). Attaching processes remap the region at the **same virtual address** as the creator (`map_base_addr` in the region header). No offset translation is performed at runtime.

---

## CLI options (`-o` extended options)

| Token | Argument | Effect |
|-------|----------|--------|
| `shm_name` | string | Region path: POSIX name (`/memcached_shm`) or DAX device (`/dev/dax0.0`). **Required** to enable SHM mode. |
| `shm_backend` | `posix` \| `dax` | Storage backend (default: `posix`). Both processes must use the same value. |
| `shm_size` | integer (MB) | Slab arena size. `0` → use `-m maxbytes`. For DAX, must fit within device size (checked at create). |
| `shm_create` | none | This process creates and initialises the region (loader). |
| `shm_attach` | none | This process attaches to an existing region (worker). |

Parsed in `memcached.c` (`settings_init`, option switch ~lines 5610–5630, init block ~5975–6152).

`settings` fields in `memcached.h`:

```c
char  *shm_name;
size_t shm_size;
bool   shm_create;
int    shm_backend;   /* SHM_BACKEND_POSIX (0) or SHM_BACKEND_DAX (1) from shm_alloc.h */
```

Global handle:

```c
extern struct mc_shm_backend *g_shm_backend;   /* memcached.h */
```

---

## Initialisation order (`memcached.c` `main`)

When `settings.shm_name != NULL`:

1. **`shm_backend_create()`** (creator) or **`shm_backend_attach()`** (attacher)  
   Allocates/opens the `shm_alloc` region and locates `slab_arena`, `hashtable`, `shm_ctrl`.

2. **`slabs_shm_setup(b, is_creator)`**  
   Redirects `slabclass`, `mem_*` pointers, and `slabs_lock` into `shm_control_block_t`.

3. **`items_shm_setup(b)`**  
   Redirects LRU heads/tails/sizes and CAS state into the control block.

4. **`assoc_init(hashpower)`**  
   In SHM mode: uses `ctrl->primary_hashtable` and `ctrl->hashpower` (no `calloc`).

5. **`slabs_init(slab_limit, …, slab_arena, reuse_mem)`**  
   - Creator: `reuse_mem=false`, preallocates one slab page per class into the shared arena.  
   - Attacher: `reuse_mem=true`, skips preallocation (arena already populated).  
   - `slab_limit` = `b->slab_size` (bytes), not `-m` alone.

6. Thread / LRU / crawler startup (`memcached_thread_init`, etc.).

7. **Creator only:** `ctrl->initialized = 1` — unblocks attachers.

---

## `shm_alloc` API calls used by memcached

All calls are in `shm_backend.c`. User id `1` and `SHM_PERM_DEFAULT` / `SHM_PERM_ADMIN` are used throughout.

### Region open (create path)

**POSIX:**

```c
shm_region_open_opts_t opts = {
    .backend      = SHM_BACKEND_POSIX,
    .flags        = SHM_OPEN_CREATE,
    .dir_capacity = 16,
};
shm_region_open(name, total, &opts, &b->region);
```

**DAX:**

```c
shm_region_open_opts_t opts = {
    .backend      = SHM_BACKEND_DAX,
    .flags        = SHM_OPEN_CREATE,
    .dir_capacity = 16,
};
/* size=0 → fstat device, mmap entire device; then verify total <= dev_size */
shm_region_open(name, 0, &opts, &b->region);
```

| Parameter | POSIX | DAX |
|-----------|-------|-----|
| `name` | e.g. `"/memcached_shm"` | e.g. `"/dev/dax0.0"` |
| `size` (create) | `region_total_size(...)` — `ftruncate` to this size | `0` — full device size from `/sys/bus/dax/devices/<name>/size` (not `fstat`) |
| `opts.backend` | `SHM_BACKEND_POSIX` | `SHM_BACKEND_DAX` |
| `opts.flags` | `SHM_OPEN_CREATE` | `SHM_OPEN_CREATE` |
| Underlying API | `shm_open()` + `ftruncate()` | `open(O_RDWR)` — no truncate |
| Size check | N/A (region sized exactly) | `region_total_size(slab_size, ht_power) <= shm_region_size()` or `ENOSPC` |

### Heap allocations inside the region

Three objects are registered in the directory:

```c
// 1. Slab arena — all item/key/value bytes
shm_alloc(region, slab_size, "slab_arena", 1, SHM_PERM_DEFAULT, 1, &slab_id, &slab_off);
b->slab_arena = shm_ptr(region, slab_off, 1, SHM_PERM_DEFAULT, SHM_PERM_READ | SHM_PERM_WRITE);

// 2. Hash table — primary_hashtable bucket array
shm_alloc(region, ht_size, "hashtable", 1, SHM_PERM_DEFAULT, 2, &ht_id, &ht_off);
b->ht_arena = shm_ptr(region, ht_off, 1, SHM_PERM_DEFAULT, SHM_PERM_READ | SHM_PERM_WRITE);

// 3. Control block — metadata + locks + slabclass array
shm_alloc(region, sizeof(shm_control_block_t), "shm_ctrl", 1, SHM_PERM_DEFAULT, 3, &ctrl_id, &ctrl_off);
b->ctrl = (shm_control_block_t *)shm_ptr(region, ctrl_off, 1, SHM_PERM_DEFAULT, SHM_PERM_READ | SHM_PERM_WRITE);
```

| API | Purpose |
|-----|---------|
| `shm_alloc()` | Allocate `size` bytes in the region heap; register in directory with optional `name` and `type_tag` |
| `shm_ptr()` | Convert `shm_off_t` offset to local virtual address after permission check |

### Region open (attach path)

```c
shm_region_open_opts_t opts = {
    .backend = backend,   /* SHM_BACKEND_POSIX or SHM_BACKEND_DAX — must match creator */
    .flags   = 0,
};
shm_region_open(name, 0, &opts, &b->region);
```

| Parameter | Value | Meaning |
|-----------|-------|---------|
| `size` | `0` | Size read from existing object (POSIX `fstat`) or DAX device |
| `flags` | `0` | Do not create; attach only |

### Lookup by name (attach path)

```c
shm_lookup_by_name(region, "shm_ctrl",    1, SHM_PERM_ADMIN, &ctrl_id, &ctrl_off);
shm_lookup_by_name(region, "slab_arena",  1, SHM_PERM_ADMIN, &slab_id, &slab_off);
shm_lookup_by_name(region, "hashtable",   1, SHM_PERM_ADMIN, &ht_id,    &ht_off);
```

Each result is followed by `shm_ptr()` to obtain the local pointer.

### Close

```c
shm_region_close(b->region, unlink);   /* shm_backend_destroy() */
```

### Fixed virtual address (inside `allocator/src/shm_alloc.c`)

On **create**, after `mmap`:

```c
region_hdr(reg)->map_base_addr = (uint64_t)(uintptr_t)reg->base;
```

On **attach**, before use:

```c
mmap((void *)(uintptr_t)ph->map_base_addr, region_size,
     PROT_READ|PROT_WRITE, MAP_SHARED | MAP_FIXED_NOREPLACE, fd, 0);
```

Falls back to ordinary `mmap` if `MAP_FIXED_NOREPLACE` is unavailable or fails.

### Not used by memcached (available for extensions)

- `shm_free_id`, `shm_resize`, `shm_dir_next`, `shm_lookup` (by id)
- `SHM_OPEN_ENFORCE_NS`, `SHM_OPEN_ENFORCE_CGROUP`
- C++ wrappers in `allocator/include/shm_alloc.hpp`

DAX is selected via `-o shm_backend=dax`; memcached passes `SHM_BACKEND_DAX` into `shm_backend_create()` / `shm_backend_attach()`.

See `allocator/README.md` for the full allocator API.

---

## File reference

### New files

| File | Role |
|------|------|
| `allocator/include/shm_alloc.h` | Public C API (included by `shm_backend.h`) |
| `allocator/src/shm_alloc.c` | Region/heap/directory implementation |
| `allocator/src/shm_ns.c` | IPC namespace + cgroup fingerprinting |
| `slabs_types.h` | `slabclass_t`, `SLABS_SHM_MAX_LIST` (1024) |
| `shm_backend.h` | `shm_control_block_t`, `mc_shm_backend_t`, API |
| `shm_backend.c` | `shm_backend_create/attach/destroy` |

### `shm_control_block_t` (`shm_backend.h` lines 55–98)

| Field | Shared? | Used by |
|-------|---------|---------|
| `initialized` | yes | Creator sets `1` when ready; attacher spins until set |
| `power_largest` | yes | Highest slab class id (`slabs_init`) |
| `mem_base` | yes | Start of slab arena |
| `mem_current` | yes | Bump pointer for `memory_allocate()` |
| `mem_avail` | yes | Bytes left in arena |
| `mem_limit` | yes | Total arena size |
| `mem_malloced` | yes | Bytes allocated from arena |
| `mem_limit_reached` | yes | Set when arena exhausted |
| `primary_hashtable` | yes | Pointer to `hashtable` object (stored as `void*`) |
| `hashpower` | yes | log2(bucket count) |
| `item_lock_hashpower` | yes | Fixed `13` → 8192 item locks |
| `item_lock_count` | yes | `1 << 13` |
| `cas_id` | yes | Global CAS counter |
| `lru_heads[]`, `lru_tails[]` | yes | Per-class LRU lists |
| `lru_sizes[]`, `lru_sizes_bytes[]` | yes | LRU statistics |
| `slabclass[]` | yes | All slab class metadata |
| `sc_slab_list[][]` | yes | Inline backing store for `slab_list` pointers |
| `slabs_lock` | yes | Process-shared mutex |
| `cas_id_lock` | yes | Process-shared mutex |
| `item_locks[]` | yes | 8192 process-shared mutexes |
| `lru_locks[]` | yes | 256 process-shared mutexes |

### `mc_shm_backend_t` (per-process, not in SHM)

| Field | Meaning |
|-------|---------|
| `region` | `shm_region_t*` handle |
| `ctrl` | Pointer to shared `shm_ctrl` object |
| `slab_arena` | Cached pointer to slab heap |
| `slab_size` | Arena size in bytes |
| `ht_arena` | Cached pointer to hash buckets |
| `hashtable_power` | Copy of `hashpower` |
| `is_creator` | `true` if this process called `shm_backend_create` |

### `shm_backend.c` — function-by-function

#### `init_pshared_mutex(m)` (lines 36–44)

```c
pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
pthread_mutex_init(m, &attr);
```

Required so mutexes in the control block work across processes.

#### `init_ctrl(ctrl, slab_arena, slab_size, ht_arena, ht_power)` (lines 46–84)

- `memset` control block to zero.
- Set `mem_base`, `mem_current`, `mem_avail`, `mem_limit`, `primary_hashtable`, `hashpower`, `cas_id = 1`.
- For each slab class `i`: `slabclass[i].slab_list = sc_slab_list[i]`, `list_size = SLABS_SHM_MAX_LIST`.
- Initialise all process-shared mutexes.

#### `region_total_size(slab_size, ht_power)` (lines 87–96)

```
total = sizeof(shm_control_block_t)
      + slab_size
      + (2^ht_power) * sizeof(void*)
      + 8 MiB overhead
rounded up to 2 MiB
```

#### `shm_backend_create(name, slab_size, hashtable_power, backend, out)` (lines 100–186)

Sequence documented in [shm_alloc API calls](#shm_alloc-api-calls-used-by-memcached) above, then `init_ctrl()`.

- **`backend`:** `SHM_BACKEND_POSIX` or `SHM_BACKEND_DAX` (from CLI `-o shm_backend=…`).
- **DAX create:** passes `size=0` to `shm_region_open` so the full device is mapped; fails with `ENOSPC` if `region_total_size(slab_size, ht_power) > shm_region_size()`.

#### `shm_backend_attach(name, backend, out)` (lines 188–265)

Opens region with matching `backend`, `shm_lookup_by_name` × 3, waits on `ctrl->initialized` (1 ms sleep, 30 s timeout).

#### `shm_backend_destroy(b, unlink)` (lines 248–253)

`shm_region_close` + `free(b)`.

---

### `slabs.c` — shared memory integration

#### Local vs shared state (lines 29–64)

- `slabclass` → points to `_local_slabclass[]` or `ctrl->slabclass`.
- `mem_limit`, `mem_malloced`, `mem_base`, `mem_current`, `mem_avail`, `power_largest` → pointer indirection + macros.
- `slabs_lock_p` → local mutex or `&ctrl->slabs_lock`.

#### `slabs_init()` SHM paths (lines 208–296)

When `mem_base_external != NULL` (SHM arena):

- `mem_base = mem_base_external`
- Creator (`reuse_mem=false`): `mem_current = mem_base`, `mem_avail = mem_limit`
- Attacher (`reuse_mem=true`): `mem_current = mem_base + mem_limit`, `mem_avail = 0` (arena full)

**Critical fix:** After `memset(slabclass, 0, …)` the code re-wires SHM lists:

```c
if (g_shm_backend) {
    shm_control_block_t *ctrl = ((struct mc_shm_backend *)g_shm_backend)->ctrl;
    for (int j = 0; j < MAX_NUMBER_OF_SLAB_CLASSES; j++) {
        slabclass[j].slab_list = ctrl->sc_slab_list[j];
        slabclass[j].list_size = SLABS_SHM_MAX_LIST;
    }
}
```

Without this, `list_size` stays `0`, `do_grow_slab_list()` treats the list as full, and preallocation fails with:

```
slabs: slab_list full for class N (limit 0)
Error while preallocating slab memory!
```

#### `memory_allocate(size)` (lines 625–653)

Bump allocator from `[mem_current, mem_base + mem_limit)`:

- Aligns `size` to `CHUNK_ALIGN_BYTES`
- Advances `mem_current`, decreases `mem_avail`
- Increments `mem_malloced`

All slab pages (items/keys/values) come from this arena in SHM mode.

#### `do_grow_slab_list(id)` (lines 337–356)

- SHM: `slab_list` is fixed at `SLABS_SHM_MAX_LIST` (1024) entries; **no `realloc`**
- Full when `p->slabs >= p->list_size` (and `list_size > 0`)

#### `slabs_shm_setup(b, is_creator)` (lines 858–909)

Redirects pointers into `ctrl`; uses `#pragma push_macro` when assigning `&ctrl->mem_limit` etc. to avoid macro expansion on field names.

#### `slabs_shm_restore_state(b)` (lines 913)

No-op placeholder for attach path after `slabs_init(reuse_mem=true)`.

---

### `items.c` — LRU and CAS in SHM

#### Indirection (lines 56–85)

- `g_heads`, `g_tails`, `g_sizes`, `g_sizes_bytes` → arrays in control block or local statics.
- Macros `heads`, `tails`, `sizes`, `sizes_bytes`, `cas_id` keep the rest of the file unchanged.
- `cas_id_lock_p` → `ctrl->cas_id_lock` in SHM mode.

#### `items_shm_setup(b)` (lines 91–114)

Points globals at `ctrl->lru_*` and `ctrl->cas_id` / `cas_id_lock`.

---

### `assoc.c`

#### `assoc_init()` (lines 56–72)

If `g_shm_backend`:

```c
hashpower         = g_shm_backend->ctrl->hashpower;
primary_hashtable = (item **)g_shm_backend->ctrl->primary_hashtable;
```

No dynamic allocation; bucket array lives in the `hashtable` shm object.

#### `assoc_expand()` (lines 140–144)

Returns immediately in SHM mode — table size is fixed at create time (`hashpower` from `-o hashpower=N` or default).

---

### `thread.c` — `memcached_thread_init()` (lines 1091–1109)

If `g_shm_backend`:

```c
lru_locks           = ctrl->lru_locks;
item_locks          = ctrl->item_locks;
item_lock_count     = ctrl->item_lock_count;
item_lock_hashpower = ctrl->item_lock_hashpower;
```

Skips local `pthread_mutex_init` for those arrays (already initialised in `init_ctrl`).

---

## Data flow: storing a key/value

1. Client sends `set key …` to either port.
2. Worker thread hashes key → `assoc_find` reads `primary_hashtable` in SHM.
3. `do_item_alloc` → `slabs_alloc` → `do_slabs_newslab` → `memory_allocate` bumps `mem_current` in **shared** control block (under `slabs_lock`).
4. Item struct (key + value inline) lives in **slab_arena**; pointers in item/link structures are valid in both processes.
5. `do_item_link` inserts into hash chain and LRU list under item/LRU locks in SHM.
6. Second process sees the item immediately on `get` (same physical memory).

---

## Limitations

| Topic | Behavior |
|-------|----------|
| Hash table growth | Disabled (`assoc_expand` no-op) |
| `slab_list` growth | Fixed 1024 pages/class (`SLABS_SHM_MAX_LIST`) |
| DAX device size | Fixed at hardware/config time; `shm_size` must fit in device |
| DAX re-init | Creator with `shm_create` overwrites region header; device must be cleared for a fresh run |
| extstore / `-e` | Not integrated with SHM backend |
| Restart / `-e memory_file` | Incompatible with SHM mode |
| Pointer safety | Same VA across processes depends on mmap layout (see `allocator/README.md`) |
| SHM name | Must be enabled explicitly via `-o shm_name=…` |

---

## Build system (`Makefile.am`)

Added to `memcached_SOURCES`:

- `shm_backend.c`, `shm_backend.h`, `slabs_types.h`
- `allocator/src/shm_alloc.c`, `allocator/src/shm_ns.c`, `allocator/include/shm_alloc.h`

```
memcached_CPPFLAGS += -I$(srcdir)/allocator/include
memcached_LDADD = -lrt -lpthread
```

---

## Troubleshooting

| Symptom | Cause / fix |
|---------|-------------|
| `slab_list full for class N (limit 0)` | Fixed: `slabs_init` must re-wire `slab_list` after `memset(slabclass)` |
| `shm_backend_create failed` (POSIX) | Name must start with `/`; check `/dev/shm` permissions |
| `shm_backend_create failed` (DAX) | Wrong device size (was 2 MiB): fixed by reading `/sys/bus/dax/devices/daxX.Y/size`; verify with `cat /sys/bus/dax/devices/dax0.0/size` |
| `shm_backend_create failed` (DAX) | Device too small for `shm_size` + metadata; reduce `-o shm_size` or use a larger DAX region |
| `shm_backend_create failed` (DAX) | Permission denied on `/dev/dax0.0`; run with appropriate privileges |
| Attacher times out | Creator not started, wrong `shm_backend`, or wrong device path |
| Keys missing on port 11212 | Wrong `shm_name`, or attacher started before creator finished init |
| `mmap MAP_FIXED` failed | ASLR/layout conflict; check `map_base_addr` in region header |
| OOM despite large `-m` | Set `shm_size` ≥ `-m`; `slabs_init` uses `b->slab_size` in SHM mode |

---

## Related documentation

- `allocator/README.md` — full **shm_alloc** library API, layout, permissions, DAX backend
- `README.md` — upstream memcached build/run
