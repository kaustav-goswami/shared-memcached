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

Requires a configured DAX device (e.g. `/dev/dax0.0`) at least as large as `shm_size` (total window). The device must be **zeroed or freshly formatted** before the first `shm_create` — the creator initialises the `shm_alloc` region header on the device. Configure gem5’s DAX/cache window to **`shm_size`**, not the raw device size, if they differ.

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
- `shm_size` is in **megabytes** (parsed by `-o shm_size=N`) and is the **total** shared region / gem5 DAX window, not the raw item-arena size.
- `-m` defaults `shm_size` when unset; the usable slab arena is slightly smaller (hashtable + control + ~8 MiB metadata reserve are carved out).
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
| `shm_size` | integer (MB) | **Total** shared region size (the DAX/gem5 address window). `0` → use `-m maxbytes`. The slab arena is carved from the remainder after hashtable + control + metadata (+ guards). Must fit within the DAX device. |
| `shm_create` | none | This process creates and initialises the region (loader). |
| `shm_attach` | none | This process attaches to an existing region (worker). |
| `shm_guard` | optional pages | `shm_guard` or `shm_guard=N` — PROT_NONE pages at the end of the mapped window (default N=16). Catches OOB pointer walks; stored in the region header for attachers. |

Parsed in `memcached.c` (`settings_init`, option switch ~lines 5610–5630, init block ~5975–6152).

`settings` fields in `memcached.h`:

```c
char  *shm_name;
size_t shm_size;      /* total shared region bytes (DAX/gem5 window) */
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
/* Map exactly region_size bytes (≤ device); do not map the whole device. */
shm_region_open(name, region_size, &opts, &b->region);
```

| Parameter | POSIX | DAX |
|-----------|-------|-----|
| `name` | e.g. `"/memcached_shm"` | e.g. `"/dev/dax0.0"` |
| `size` (create) | `region_size` (`shm_size`) — `ftruncate` to this size | `region_size` — mmap first N bytes; must be ≤ device (`ENOSPC` if larger) |
| `opts.backend` | `SHM_BACKEND_POSIX` | `SHM_BACKEND_DAX` |
| `opts.flags` | `SHM_OPEN_CREATE` | `SHM_OPEN_CREATE` |
| Underlying API | `shm_open()` + `ftruncate()` | `open(O_RDWR)` — no truncate |
| Size check | region sized exactly | `region_size <= /sys/bus/dax/devices/<name>/size` |

### Heap allocations inside the region

`slab_size` is carved from `region_size` first; then three objects are registered:

```c
slab_size = region_size - ht_size - sizeof(shm_control_block_t) - 8MiB_reserve;

// 1. Slab arena — all item/key/value bytes
shm_alloc(region, slab_size, "slab_arena", 1, SHM_PERM_DEFAULT, 1, &slab_id, &slab_off);
b->slab_arena = shm_ptr(region, slab_off, 1, SHM_PERM_DEFAULT, SHM_PERM_READ | SHM_PERM_WRITE);

// 2. Hash table — primary_hashtable bucket array
shm_alloc(region, ht_size, "hashtable", 1, SHM_PERM_DEFAULT, 2, &ht_id, &ht_off);
b->ht_arena = shm_ptr(region, ht_off, 1, SHM_PERM_DEFAULT, SHM_PERM_READ | SHM_PERM_WRITE);

// 3. Control block — metadata + locks + slabclass array
shm_alloc(region, sizeof(shm_control_block_t), "shm_ctrl", 1, SHM_PERM_DEFAULT, 3, &ctrl_id, &ctrl_off);
b->ctrl = (shm_control_block_t *)shm_ptr(region, ctrl_off, 1, SHM_PERM_DEFAULT, SHM_PERM_READ | SHM_PERM_WRITE);
/* assert: end offset of each object ≤ region_size */
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

Required because memcached stores raw `item *` pointers in the shared region.

On **create**, after `mmap`:

```c
region_hdr(reg)->map_base_addr = (uint64_t)(uintptr_t)reg->base;
```

On **attach**: peek one page for `region_size` / `map_base_addr`, then

```c
mmap((void *)(uintptr_t)ph->map_base_addr, region_size,
     PROT_READ|PROT_WRITE, MAP_SHARED | MAP_FIXED_NOREPLACE, fd, 0);
```

If the fixed map fails, attach falls back to a relocated mapping and prints a
warning (offset-based shm_alloc clients still work).  `shm_backend_attach`
then **refuses** to start memcached unless `ctrl` absolute pointers match the
local `shm_ptr` results.

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
| `slab_size` | Carved arena size in bytes (≤ `region_size`) |
| `region_size` | Total mapped shared region (`shm_size`) |
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

#### `slab_arena_from_region(region_size, ht_power)`

```
overhead   = sizeof(shm_control_block_t)
           + (2^ht_power) * sizeof(void*)
           + 8 MiB metadata reserve
slab_arena = region_size - overhead     (fails if region_size ≤ overhead)
```

The 8 MiB reserve covers the shm_alloc region header, object directory,
per-object block headers, alignment, and free-list slack so that
`slab_end_offset ≤ region_size` always holds.

#### `shm_backend_create(name, region_size, hashtable_power, backend, out)`

Sequence documented in [shm_alloc API calls](#shm_alloc-api-calls-used-by-memcached) above, then `init_ctrl()`.

- **`region_size`:** total shared window (`settings.shm_size`).
- **`backend`:** `SHM_BACKEND_POSIX` or `SHM_BACKEND_DAX` (from CLI `-o shm_backend=…`).
- **Both backends:** `shm_region_open(name, region_size, …)` — map exactly the window.
- **DAX:** fails with `ENOSPC` if `region_size` exceeds the device capacity from sysfs.
- **Post-alloc check:** asserts slab / hashtable / ctrl end offsets are `< region_size`.

#### `shm_backend_attach(name, backend, out)`

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
| DAX device size | Fixed at hardware/config time; `shm_size` (total window) must fit in device |
| Item capacity | Slightly less than `shm_size` (hashtable + ctrl + 8 MiB reserve carved out) |
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

## gem5 / DAX address-bounds bugs

### Symptom A — layout past `shm_size`

With `-m 512 -o shm_size=512,shm_backend=dax,…` on a 1 GiB `/dev/dax` device,
gem5 crashes on an invalid cache-block address. Debug prints show a
DAX-relative offset **greater than 512 MiB**.

**Cause:** `shm_size` was treated as the slab arena size only, then placed
after shm_alloc metadata, so items/hashtable/ctrl sat past the window. DAX
create also mapped the **entire device**.

**Fix:** `shm_size` is the total mapped window; the slab arena is carved as
`shm_size − ht − ctrl − 8 MiB reserve − guards`. Create maps exactly `shm_size` bytes.

### Symptom B — attacher VA mismatch / over-map (e.g. PA `0x1839ADF20`)

With DAX at `[0x100000000, 0x200000000)` and `-m 2048 -o shm_size=2048`, gem5
sees a packet for `0x1839ADF20` / `0x1839ADF28` (offset ≈ **2105 MiB**, ~58 MiB
past the 2 GiB window) while YCSB is running.

**Cause (compound):**

1. Memcached embeds **raw C pointers** in the shared region. Attach used
   `mmap(NULL)` and/or mapped the **whole 4 GiB device**, so ASLR skew turned
   creator pointers into wrong DAX offsets.
2. Even with a 2 GiB request, some gem5/guest stacks still establish a VMA
   larger than `shm_size`. Offsets past 2 GiB stay reachable and produce
   packets into the unused half of the 4 GiB device (exactly the
   `0x1839ADF2x` class of crashes). Falling back to an ASLR VA near libc
   (`0x7f63…`) makes this look like a normal mutex `cmpxchg` in the trace.

**Fix (current):**

- Region header stores `map_base_addr` + `guard_pages` (shm_alloc **version 4**).
- Create/attach **require** a fixed high VA (`0x4000000000` …); DAX never falls
  back to `mmap(NULL)`.
- Map exactly `shm_size`, then **chop** any over-mapped tail (`munmap` past the
  window) and verify `/proc/self/maps`.
- Optional `-o shm_guard[=N]`: `mprotect(PROT_NONE)` on the last N pages of the
  window plus anonymous `PROT_NONE` pages immediately after the VMA so OOB
  walks SIGSEGV in the guest instead of hitting gem5.

After upgrade: **rebuild memcached + allocator**, **wipe/re-create the DAX
region** (`shm_create` — old headers are version ≤ 3), confirm stderr shows
`max_end <= region`, `VA=… (fixed)`, `vma=<shm_size>`, and (if enabled)
`guards active`.

### Recommended gem5 command line

```bash
# Creator / loader — 2 GiB window on a 4 GiB device, with guards
./memcached -p 11211 -U 0 -m 2048 \
  -o shm_backend=dax,shm_name=/dev/dax0.0,shm_size=2048,shm_create,hashpower=20,shm_guard=256

# Attacher / worker (guards come from the region header)
./memcached -p 11212 -U 0 -m 2048 \
  -o shm_backend=dax,shm_name=/dev/dax0.0,shm_attach
```

Keep gem5’s accessible DAX window **≥ `shm_size`** (here 2 GiB → through
`0x180000000`). Do not configure the guest to use the full 4 GiB device when
gem5 only wires the first 2 GiB.

---

## Address-bounds code audit (line-by-line pass over allocator + shm integration)

This section documents a dedicated audit of every address/offset calculation
in `allocator/src/shm_alloc.c`, `shm_backend.c`, `slabs.c`, `items.c`,
`assoc.c`, and `thread.c`, triggered by crashes that reproduced even with
**zero items** (memaslap with no workload) and **before any GET/SET**
arrived from YCSB (during connection/startup). Goal: find and patch every
place a computed address could land outside the region/arena it belongs to,
independent of the `shm_size` vs. gem5-window sizing issues already covered
above.

### Methodology

1. Read every pointer/offset helper in `shm_alloc.c` (`blk_at_hoff`,
   `blk_of_poff`, `hoff_of_blk`, `poff_of_blk`, `dir_slot`, `heap_size`,
   `region_init`, `install_guard_pages`) and every call site that converts a
   `shm_off_t` into a raw pointer.
2. Traced the full memcached shm startup sequence in `memcached.c` (`main`)
   to see what runs unconditionally at boot before any client I/O:
   `shm_backend_create` → `slabs_shm_setup` → `items_shm_setup` →
   `assoc_init` → `slabs_init` (→ `slabs_preallocate` → `do_slabs_newslab` →
   `memory_allocate`) → `memcached_thread_init` → `init_lru_crawler`.
3. Checked every redirect of "shared vs. local" state (`slabs.c`'s
   `_mem_*_p` pointer indirection, `items.c`'s `g_heads`/`g_cas_id_p`,
   `assoc.c`'s `hashpower`/`primary_hashtable`, `thread.c`'s
   `item_locks`/`lru_locks`) for ordering bugs and index bounds.
4. Ruled out `int`/`uint32_t` truncation of the ~4 GiB region size (all
   offsets in `shm_alloc.h`/`shm_backend.h` are already `size_t`/`shm_off_t`
   (`uint64_t`); no 32-bit-truncated byte offsets were found).
5. Applied fixes, then verified with `allocator/tests` (`test_basic`,
   `test_resize`, `test_multihost` — all pass) and an end-to-end memcached
   smoke test (POSIX shm backend: `shm_create`, `slabs_preallocate` at
   startup, then `set`/`get`/`incr`/`delete` over TCP) after rebuilding with
   `-Wall -Wextra -pedantic` (no new warnings).

### Bug 1 (real, exploitable) — `shm_ptr()` / `shm_block_info()` / `shm_block_set_perms()` dereferenced caller-supplied offsets with **no bounds check**

`blk_of_poff(r, poff)` was pure pointer arithmetic:

```c
return (shm_block_hdr_t *)(heap_base(r) + poff - sizeof(shm_block_hdr_t));
```

`shm_ptr()` called this and then unconditionally read `b->magic` inside
`check_block_access()`. Any offset that was stale, corrupted, or the
`SHM_OFF_NULL` sentinel (`UINT64_MAX`) turned directly into a pointer that
could land **far outside** `[heap_base, heap_base + heap_size)` and get
dereferenced immediately — a classic "address calculation" OOB bug. On a
tightly-bounded DAX/gem5 window this reliably faults (matches the reported
symptom); on POSIX/DRAM the same bad offset can instead silently hit
unrelated heap memory, which is consistent with "normal memcached works,
DAX crashes" even though the *root cause* here is not DAX-specific.

**Fix:** added `blk_of_poff_checked()` as the single choke point every
`shm_off_t` must pass through before being dereferenced. It rejects
`SHM_OFF_NULL`, any offset that would place the block header outside the
mapped heap, and — once the header is confirmed readable — any block whose
own `total_size` claims to extend past the end of the heap (catches a
corrupted/poisoned header before its `payload_cap` is trusted for further
`memset`/`memcpy`). `shm_ptr()`, `shm_block_info()`, and
`shm_block_set_perms()` now use it and return `NULL`/`EINVAL` instead of
dereferencing a wild pointer. `shm_free()` and `shm_resize()` (whose offsets
come from the trusted internal directory) were also switched to the
checked variant as a defense-in-depth net against a corrupted directory
slot.

### Bug 2 (real, latent) — `memory_allocate()` checked `mem_avail` **before** aligning the request

```c
ret = mem_current;
if (size > mem_avail) return NULL;          /* checked BEFORE alignment */
if (size % CHUNK_ALIGN_BYTES)
    size += CHUNK_ALIGN_BYTES - (size % CHUNK_ALIGN_BYTES);
mem_current = ((char*)mem_current) + size;  /* bumped AFTER alignment  */
```

If a caller ever requested a size that was not already a multiple of
`CHUNK_ALIGN_BYTES`, the check used the smaller pre-alignment size while the
actual bump used the larger, rounded-up size — `mem_current` could advance
past `mem_base + mem_limit` (the end of the slab arena, which in shm mode
*is* the shm/DAX-backed window) without `memory_allocate()` ever returning
`NULL`. This function is called from `slabs_preallocate()` **at startup**,
before any client connects — matching "crashes on start with no workload".
With the current default settings (`slab_page_size`, per-class chunk sizes)
the passed-in size is always already aligned, so this was latent rather
than actively firing, but it is a genuine bounds-check-ordering bug and is
now fixed: align first, then check against `mem_avail`, so the bump can
never exceed what was verified available.

### Bug 3 (defensive) — `region_init()` could silently underflow `heap_sz` for undersized regions

```c
if (total_size <= data_off + guard_bytes + BLOCK_MIN) {
    guard_bytes = 0; guard_pages = 0;      /* disables guards and continues */
}
...
size_t heap_sz = total_size - data_off - guard_bytes;   /* size_t underflow if total_size <= data_off */
```

Disabling guards does not guarantee `total_size > data_off`; a region only
barely larger than the header + object directory (not reachable through the
current memcached 4000 MiB configuration, but reachable through the general
`shm_alloc` API with a large `dir_capacity`) would underflow `heap_sz` to a
huge `size_t`, and the allocator would hand out a "free block" claiming to
be several exabytes — every subsequent `shm_alloc()` would then compute
pointers far outside the mapping. **Fix:** `shm_region_open()` now validates
`size > header + directory + guard + BLOCK_MIN` up front and returns
`EINVAL` with a descriptive message; `region_init()` itself was also
hardened to clamp to an empty (unusable, but never wraparound) heap if this
is ever reached through another path.

### Observation (not a bug) — DAX in-region trailing guard often fails to apply

`install_guard_pages()` tries to `munmap()` + re-`mmap(PROT_NONE)` the last
`shm_guard` pages *inside* the DAX window. On the kernels/dax drivers this
was tested against, the `munmap()` of a slice of a device-DAX VMA returns
`EINVAL`, so that in-region guard silently does **not** get applied — only
the post-VMA guard (immediately after the mapping) is active. The allocator
still never *legally* hands out bytes in that trailing region (`heap_size()`
excludes it), so this by itself does not cause an OOB write — but it does
mean a stray OOB write that lands in exactly that window will silently
succeed instead of SIGSEGV'ing, making such bugs harder to catch. The
startup log now says so explicitly instead of claiming "guards active":

```
shm_alloc: WARNING in-region guard NOT enforced — bytes N..M at 0x... remain
live/writable DAX memory (heap accounting excludes them, but a stray OOB
write there will not fault); only the post-VMA guard is active
```

### Reviewed and found safe (no change needed)

- **`assoc_expand()`** is unconditionally disabled in shm mode
  (`if (g_shm_backend) return;`), so `hashpower` / `primary_hashtable`
  (copied once into process-local statics rather than pointer-indirected)
  never actually diverge from the control block today. This is fragile if a
  future code path called `assoc_expand()` unconditionally, but is not
  implicated in the current crashes.
- **`item_lock()` / `item_trylock()` / `item_unlock()`** always mask with
  `hashmask(item_lock_hashpower)` where `item_lock_hashpower` is fixed at
  `SHM_ITEM_LOCK_HASHPOWER` (13) in shm mode and `item_locks` is sized
  `1 << 13`; any `hv` value is safe by construction. `memcached_thread_init()`
  redirects `item_locks`/`item_lock_hashpower` before any worker thread is
  spawned, so there is no ordering window where a thread could index with
  stale bounds.
- **`shm_resize()`'s in-place-growth check**
  (`next_ho + sizeof(shm_block_hdr_t) < heap_size(region)`) is conservative
  (refuses growth one byte earlier than strictly necessary) but never reads
  past the heap — the strict `<` only rejects a valid case, it does not
  admit an invalid one.
- No `uint32_t`-truncated byte offsets were found for the ~4 GiB region
  size case; every offset/size in the allocator and `shm_backend.h` is
  `size_t` or the explicit 64-bit `shm_off_t`.

### Caveat: this audit does not rule out the previously-identified gem5 issue

A separate, earlier investigation in this same session traced a crash to
gem5's **classic cache hierarchy mishandling locked RMW (`lock cmpxchg`)
completions** — reproduced even for a plain **DRAM** address with caches
enabled, and resolved by removing caches entirely. That is a gem5-side bug
in the simulated memory system, not an address-calculation bug in
memcached/`shm_alloc`. Since every process-shared mutex lock (`slabs_lock`,
`cas_id_lock`, `item_locks[]`, the `shm_alloc` region mutex) compiles to a
`lock cmpxchg` on `pthread_mutex_lock()`, **any** of the locking that
happens during connection setup or startup (e.g. `region_lock()` inside
`shm_alloc()` while carving `slab_arena`/`hashtable`/`shm_ctrl`, or
`slabs_lock` during `slabs_preallocate()`) can trip that gem5 bug
independent of whether the target address is in-bounds. If the fixes in
this section do not fully resolve the crash, the next step is to correlate
the exact crashing PA/VA against the ranges validated here — if it falls
inside a legitimately-owned block, the remaining cause is the gem5 cache
LockedRMW path, not memcached.

---

## Troubleshooting

| Symptom | Cause / fix |
|---------|-------------|
| gem5 invalid DAX address / offset > `shm_size` | See [gem5 / DAX address-bounds bugs](#gem5--dax-address-bounds-bugs); rebuild guest binary + allocator, wipe DAX, `shm_create`, confirm `vma=` equals `shm_size` and attach is `fixed` |
| `shm_alloc: chopped N bytes past window` | Over-map detected and removed — good; if it keeps recurring, check gem5 DAX mmap |
| SIGSEGV in guard page | Software OOB past the usable window; enable/keep `shm_guard` and fix the pointer source |
| `attach FAILED — cannot map at creator VA` | Fixed VA busy; free `0x4000000000` or disable ASLR for the attacher |
| `shm_backend_attach: creator/attacher VA mismatch` | Attacher did not get creator `map_base_addr`; never run two creators |
| `slab_list full for class N (limit 0)` | Fixed: `slabs_init` must re-wire `slab_list` after `memset(slabclass)` |
| `shm_backend_create failed` (POSIX) | Name must start with `/`; check `/dev/shm` permissions / size |
| `shm_backend_create failed` (DAX) | Wrong device size (was 2 MiB): fixed by reading `/sys/bus/dax/devices/daxX.Y/size`; verify with `cat /sys/bus/dax/devices/dax0.0/size` |
| `shm_backend_create failed` (DAX) | `shm_size` larger than the device; reduce `-o shm_size` or enlarge the DAX region |
| `region … is too small for hashtable + control` | Increase `shm_size` (need room for ~8 MiB reserve + ht + ctrl + guards) |
| `shm_backend_create failed` (DAX) | Permission denied on `/dev/dax0.0`; run with appropriate privileges |
| Attacher times out | Creator not started, wrong `shm_backend`, or wrong device path |
| Keys missing on port 11212 | Wrong `shm_name`, or attacher started before creator finished init |
| `mmap MAP_FIXED` failed | ASLR/layout conflict; check `map_base_addr` in region header |
| Smaller item capacity than `-m` | Expected: arena is carved from `shm_size`; check the `shm: layout …` stderr line |

---

## Related documentation

- `allocator/README.md` — full **shm_alloc** library API, layout, permissions, DAX backend
- `README.md` — upstream memcached build/run
