/*
 * Copyright 2018 WebAssembly Community Group participants
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// This file is used as a template to generate code for regular memories or for
// shared memories. For this, the file must be included after defining either
// WASM_RT_MEM_OPS or WASM_RT_MEM_OPS_SHARED.

#if defined(WASM_RT_MEM_OPS) && defined(WASM_RT_MEM_OPS_SHARED)
#error \
    "Expected only one of { WASM_RT_MEM_OPS, WASM_RT_MEM_OPS_SHARED } to be defined"
#elif !defined(WASM_RT_MEM_OPS) && !defined(WASM_RT_MEM_OPS_SHARED)
#error \
    "Expected one of { WASM_RT_MEM_OPS, WASM_RT_MEM_OPS_SHARED } to be defined"
#endif

// ruffle-wiiu: linear memory keeps spare capacity (16 MB to start, doubling), so growing rarely copies;
// blocks are kept under 1 GB so sizes fit 32-bit size_t.
#ifndef RW_MEM_MIN_CAPACITY
#define RW_MEM_MIN_CAPACITY ((uint64_t)16 << 20)
#define RW_MEM_MAX_ALLOC ((uint64_t)1 << 30)
#endif

// Shared memory operations are defined only if we have C11
#if defined(WASM_RT_MEM_OPS) || \
    (defined(WASM_RT_MEM_OPS_SHARED) && defined(WASM_RT_C11_AVAILABLE))

#ifdef WASM_RT_MEM_OPS

// Memory operations on wasm_rt_memory_t
#define MEMORY_TYPE wasm_rt_memory_t
#define MEMORY_API_NAME(name) name
#define MEMORY_CELL_TYPE uint8_t*
#define MEMORY_LOCK_VAR_INIT(name)
#define MEMORY_LOCK_AQUIRE(name)
#define MEMORY_LOCK_RELEASE(name)

#else

// Memory operations on wasm_rt_shared_memory_t
#define MEMORY_TYPE wasm_rt_shared_memory_t
#define MEMORY_API_NAME(name) name##_shared
#define MEMORY_CELL_TYPE _Atomic volatile uint8_t*

#if WASM_RT_USE_CRITICALSECTION
#define MEMORY_LOCK_VAR_INIT(name) WIN_MEMORY_LOCK_VAR_INIT(name)
#define MEMORY_LOCK_AQUIRE(name) WIN_MEMORY_LOCK_AQUIRE(name)
#define MEMORY_LOCK_RELEASE(name) WIN_MEMORY_LOCK_RELEASE(name)
#elif WASM_RT_USE_PTHREADS
#define MEMORY_LOCK_VAR_INIT(name) PTHREAD_MEMORY_LOCK_VAR_INIT(name)
#define MEMORY_LOCK_AQUIRE(name) PTHREAD_MEMORY_LOCK_AQUIRE(name)
#define MEMORY_LOCK_RELEASE(name) PTHREAD_MEMORY_LOCK_RELEASE(name)
#endif

#endif

bool MEMORY_API_NAME(wasm_rt_memory_is_default32)(const MEMORY_TYPE* memory) {
  return memory->page_size == WASM_DEFAULT_PAGE_SIZE && !memory->is64;
}

void MEMORY_API_NAME(wasm_rt_allocate_memory)(MEMORY_TYPE* memory,
                                              uint64_t initial_pages,
                                              uint64_t max_pages,
                                              bool is64,
                                              uint32_t page_size) {
  uint64_t byte_length;
  if (u64_mult_overflow(initial_pages, page_size, &byte_length)) {
    abort();
  }
  memory->size = byte_length;
  memory->pages = initial_pages;
  memory->max_pages = max_pages;
  memory->is64 = is64;
  memory->page_size = page_size;
  MEMORY_LOCK_VAR_INIT(memory->mem_lock);

#if WASM_RT_USE_MMAP
  if (MEMORY_API_NAME(wasm_rt_memory_is_default32)(memory)) {
    const uint64_t mmap_size =
        get_alloc_size_for_mmap_default32(memory->max_pages);
    uint8_t* addr = os_mmap(mmap_size);
    if (!addr) {
      os_print_last_error("os_mmap failed.");
      abort();
    }
    uint8_t* data_end = addr + mmap_size;
#if !WABT_BIG_ENDIAN
    int ret = os_mprotect(addr, byte_length);
#else
    int ret = os_mprotect(data_end - byte_length, byte_length);
#endif
    if (ret != 0) {
      os_print_last_error("os_mprotect failed.");
      abort();
    }
    memory->data = (MEMORY_CELL_TYPE)addr;
    memory->data_end = (MEMORY_CELL_TYPE)data_end;
    return;
  }
#endif

  // ruffle-wiiu: start with spare capacity (see expand_data_allocation).
  uint64_t max_bytes = memory->max_pages * (uint64_t)page_size;
  uint64_t cap = byte_length > RW_MEM_MIN_CAPACITY ? byte_length : RW_MEM_MIN_CAPACITY;
  if (cap > max_bytes || cap > RW_MEM_MAX_ALLOC) cap = byte_length;
  uint8_t* block = NULL;
#ifdef RW_MEM_RESERVE
  // The host may reserve all it can spare at once: moving to a bigger block needs the old and the new one
  // at the same time, which no longer fit on the Wii U past 256 MB (a game ran out of memory there).
  {
    uint64_t got = 0;
    block = (uint8_t*)RW_MEM_RESERVE(cap, max_bytes, &got);
    if (block) cap = got;
  }
#endif
  if (!block) block = (uint8_t*)malloc(cap ? (size_t)cap : 1);
#ifdef WASM_RT_GROW_LOG  // ruffle-wiiu: 2 = the block reserved (new_size = its size, old_size = max)
  if (block) WASM_RT_GROW_LOG(max_bytes, cap, 2);
#endif
  if (!block) {
    abort();
  }
  memory->alloc_base = block;
  memory->capacity = cap;
#if !WABT_BIG_ENDIAN
  memory->data = (MEMORY_CELL_TYPE)block;
  memory->data_end = memory->data + byte_length;
  memset((void*)memory->data, 0, byte_length);
#else
  memory->data_end = (MEMORY_CELL_TYPE)(block + cap);
  memory->data = memory->data_end - byte_length;
  memset((void*)memory->data, 0, byte_length);
#endif
}

// Returns 0 on success
static int MEMORY_API_NAME(expand_data_allocation)(MEMORY_TYPE* memory,
                                                   uint64_t old_size,
                                                   uint64_t new_size,
                                                   uint64_t delta_size) {
#if WASM_RT_USE_MMAP
  if (MEMORY_API_NAME(wasm_rt_memory_is_default32)(memory)) {
#if !WABT_BIG_ENDIAN
    return os_mprotect((void*)(memory->data + old_size), delta_size);
#else
    return os_mprotect((void*)(memory->data_end - old_size - delta_size),
                       delta_size);
#endif
  }
#endif

  // ruffle-wiiu: grow inside the spare capacity without moving anything (big-endian memory lives at the
  // top of its block and grows downwards from data_end, which stays put). Only when that runs out is a new
  // block allocated, twice as big, and the contents copied once. Growing 64 KB at a time with realloc
  // copied the whole memory (twice, with the big-endian memmove) on every step: tens of seconds of copying
  // on the console while a game loaded.
  if (new_size <= memory->capacity) {
#ifdef RW_MEM_CAN_GROW  // GPU memory shares the block from below: stop where it starts
    if (!RW_MEM_CAN_GROW(memory->alloc_base, memory->capacity - new_size)) {
#ifdef WASM_RT_GROW_LOG
      WASM_RT_GROW_LOG(old_size, new_size, 0);
#endif
      return -1;
    }
#endif
#if !WABT_BIG_ENDIAN
    memset((void*)(memory->data + old_size), 0, delta_size);
    memory->data_end = memory->data + new_size;
#else
    memory->data = memory->data - delta_size;
    memset((void*)memory->data, 0, delta_size);
#endif
#ifdef WASM_RT_GROW_LOG
    WASM_RT_GROW_LOG(old_size, new_size, 1);
#endif
    return 0;
  }
#ifdef WASM_RT_GROW_LOG  // ruffle-wiiu: -1 = about to allocate (a breadcrumb, not logged)
  WASM_RT_GROW_LOG(old_size, new_size, -1);
#endif
  uint64_t max_bytes = memory->max_pages * (uint64_t)memory->page_size;
  uint64_t cap = memory->capacity * 2 > new_size ? memory->capacity * 2 : new_size;
  if (cap > max_bytes) cap = max_bytes;
  if (cap > RW_MEM_MAX_ALLOC) cap = new_size;
#ifdef WASM_RT_GROW_LOG  // ruffle-wiiu: the reserved block ran out (2 = log line)
  WASM_RT_GROW_LOG(memory->max_pages * (uint64_t)memory->page_size, memory->capacity, 2);
#endif
  uint8_t* block = (uint8_t*)malloc((size_t)cap);
  if (!block && cap > new_size) {
    cap = new_size;
    block = (uint8_t*)malloc((size_t)cap);
  }
#ifdef WASM_RT_GROW_LOG  // ruffle-wiiu: memory growth diagnostics
  WASM_RT_GROW_LOG(old_size, new_size, block != NULL);
#endif
  if (block == NULL) {
    return -1;
  }
#if !WABT_BIG_ENDIAN
  memcpy(block, (void*)memory->data, old_size);
  memset(block + old_size, 0, delta_size);
  memory->data = (MEMORY_CELL_TYPE)block;
  memory->data_end = memory->data + new_size;
#else
  uint8_t* end = block + cap;
  memcpy(end - old_size, (void*)memory->data, old_size);
  memset(end - new_size, 0, delta_size);
  memory->data_end = (MEMORY_CELL_TYPE)end;
  memory->data = memory->data_end - new_size;
#endif
  RW_FREE_BLOCK(memory->alloc_base);
  memory->alloc_base = block;
  memory->capacity = cap;
  return 0;
}

static uint64_t MEMORY_API_NAME(grow_memory_impl)(MEMORY_TYPE* memory,
                                                  uint64_t delta) {
  uint64_t old_pages = memory->pages;
  uint64_t new_pages = memory->pages + delta;
  if (delta == 0) {
    return old_pages;
  }
  if (new_pages < old_pages || new_pages > memory->max_pages) {
    return (uint64_t)-1;
  }

  uint64_t old_size, new_size, delta_size;
  if (u64_mult_overflow(old_pages, memory->page_size, &old_size) ||
      u64_mult_overflow(new_pages, memory->page_size, &new_size) ||
      u64_mult_overflow(delta, memory->page_size, &delta_size)) {
    return (uint64_t)-1;
  }

  int err = MEMORY_API_NAME(expand_data_allocation)(memory, old_size, new_size,
                                                    delta_size);
  if (err != 0) {
    return (uint64_t)-1;
  }

  memory->pages = new_pages;
  memory->size = new_size;
  return old_pages;
}

uint64_t MEMORY_API_NAME(wasm_rt_grow_memory)(MEMORY_TYPE* memory,
                                              uint64_t delta) {
  MEMORY_LOCK_AQUIRE(memory->mem_lock);
  uint64_t ret = MEMORY_API_NAME(grow_memory_impl)(memory, delta);
  MEMORY_LOCK_RELEASE(memory->mem_lock);
#ifdef WASM_RT_GROW_FAILED_HANDLER
  if (ret == (uint64_t)-1) {
    WASM_RT_GROW_FAILED_HANDLER();
  }
#endif
  return ret;
}

void MEMORY_API_NAME(wasm_rt_free_memory)(MEMORY_TYPE* memory) {
#if WASM_RT_USE_MMAP
  if (MEMORY_API_NAME(wasm_rt_memory_is_default32)(memory)) {
    const uint64_t mmap_size =
        get_alloc_size_for_mmap_default32(memory->max_pages);
    os_munmap((void*)memory->data, mmap_size);  // ignore error
    return;
  }
#endif
  RW_FREE_BLOCK(memory->alloc_base);
  memory->alloc_base = NULL;
  memory->capacity = 0;
}

#undef MEMORY_LOCK_RELEASE
#undef MEMORY_LOCK_AQUIRE
#undef MEMORY_LOCK_VAR_INIT
#undef MEMORY_CELL_TYPE
#undef MEMORY_API_NAME
#undef MEMORY_TYPE

#endif
