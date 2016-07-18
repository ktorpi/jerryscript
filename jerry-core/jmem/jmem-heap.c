/* Copyright 2014-2016 Samsung Electronics Co., Ltd.
 * Copyright 2016 University of Szeged.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * Heap implementation
 */

#include "jrt.h"
#include "jrt-bit-fields.h"
#include "jrt-libc-includes.h"
#include "jmem-allocator.h"
#include "jmem-config.h"
#include "jmem-heap.h"

#define JMEM_ALLOCATOR_INTERNAL
#include "jmem-allocator-internal.h"

/** \addtogroup mem Memory allocation
 * @{
 *
 * \addtogroup heap Heap
 * @{
 */

/*
 * Valgrind-related options and headers
 */
#ifdef JERRY_VALGRIND
# include "memcheck.h"

# define VALGRIND_NOACCESS_SPACE(p, s)   VALGRIND_MAKE_MEM_NOACCESS((p), (s))
# define VALGRIND_UNDEFINED_SPACE(p, s)  VALGRIND_MAKE_MEM_UNDEFINED((p), (s))
# define VALGRIND_DEFINED_SPACE(p, s)    VALGRIND_MAKE_MEM_DEFINED((p), (s))

#else /* !JERRY_VALGRIND */
# define VALGRIND_NOACCESS_SPACE(p, s)
# define VALGRIND_UNDEFINED_SPACE(p, s)
# define VALGRIND_DEFINED_SPACE(p, s)
#endif /* JERRY_VALGRIND */

#ifdef JERRY_VALGRIND_FREYA
# include "memcheck.h"

/**
 * Tells whether a pool manager allocator request is in progress.
 */
static bool valgrind_freya_mempool_request = false;

/**
 * Called by pool manager before a heap allocation or free.
 */
void jmem_heap_valgrind_freya_mempool_request (void)
{
  valgrind_freya_mempool_request = true;
} /* jmem_heap_valgrind_freya_mempool_request */

# define VALGRIND_FREYA_CHECK_MEMPOOL_REQUEST \
  bool mempool_request = valgrind_freya_mempool_request; \
  valgrind_freya_mempool_request = false

# define VALGRIND_FREYA_MALLOCLIKE_SPACE(p, s) \
  if (!mempool_request) \
  { \
    VALGRIND_MALLOCLIKE_BLOCK((p), (s), 0, 0); \
  }

# define VALGRIND_FREYA_FREELIKE_SPACE(p) \
  if (!mempool_request) \
  { \
    VALGRIND_FREELIKE_BLOCK((p), 0); \
  }

#else /* !JERRY_VALGRIND_FREYA */
# define VALGRIND_FREYA_CHECK_MEMPOOL_REQUEST
# define VALGRIND_FREYA_MALLOCLIKE_SPACE(p, s)
# define VALGRIND_FREYA_FREELIKE_SPACE(p)
#endif /* JERRY_VALGRIND_FREYA */

/* Calculate heap area size, leaving space for a pointer to the free list */
#define JMEM_HEAP_AREA_SIZE (JMEM_HEAP_SIZE - JMEM_ALIGNMENT)
#define JMEM_HEAP_END_OF_LIST ((jmem_heap_free_t *const) ~((uint32_t) 0x0))

/**
 *  Free region node
 */
typedef struct
{
  uint32_t next_offset; /**< Offset of next region in list */
  uint32_t size; /**< Size of region */
} jmem_heap_free_t;

#if UINTPTR_MAX > UINT32_MAX
#define JMEM_HEAP_GET_OFFSET_FROM_ADDR(p) ((uint32_t) ((uint8_t *) (p) - (uint8_t *) jmem_heap.area))
#define JMEM_HEAP_GET_ADDR_FROM_OFFSET(u) ((jmem_heap_free_t *) &jmem_heap.area[u])
#else /* UINTPTR_MAX <= UINT32_MAX */
/* In this case we simply store the pointer, since it fits anyway. */
#define JMEM_HEAP_GET_OFFSET_FROM_ADDR(p) ((uint32_t) (p))
#define JMEM_HEAP_GET_ADDR_FROM_OFFSET(u) ((jmem_heap_free_t *)(u))
#endif /* UINTPTR_MAX > UINT32_MAX */

/**
 * Get end of region
 */
static inline jmem_heap_free_t *  __attr_always_inline___ __attr_pure___
jmem_heap_get_region_end (jmem_heap_free_t *curr_p) /**< current region */
{
  return (jmem_heap_free_t *)((uint8_t *) curr_p + curr_p->size);
} /* jmem_heap_get_region_end */

/**
 * Heap structure
 */
typedef struct
{
  /** First node in free region list */
  jmem_heap_free_t first;

  /**
   * Heap area
   */
  uint8_t area[JMEM_HEAP_AREA_SIZE] __attribute__ ((aligned (JMEM_ALIGNMENT)));
} jmem_heap_t;

/**
 * Heap
 */
#ifndef JERRY_HEAP_SECTION_ATTR
jmem_heap_t jmem_heap;
#else /* JERRY_HEAP_SECTION_ATTR */
jmem_heap_t jmem_heap __attribute__ ((section (JERRY_HEAP_SECTION_ATTR)));
#endif /* !JERRY_HEAP_SECTION_ATTR */

/**
 * Check size of heap is corresponding to configuration
 */
JERRY_STATIC_ASSERT (sizeof (jmem_heap) <= JMEM_HEAP_SIZE,
                     size_of_mem_heap_must_be_less_than_or_equal_to_MEM_HEAP_SIZE);

/**
 * Size of allocated regions
 */
size_t jmem_heap_allocated_size;

/**
 * Current limit of heap usage, that is upon being reached, causes call of "try give memory back" callbacks
 */
size_t jmem_heap_limit;

/* This is used to speed up deallocation. */
jmem_heap_free_t *jmem_heap_list_skip_p;

#ifdef JMEM_STATS
/**
 * Heap's memory usage statistics
 */
static jmem_heap_stats_t jmem_heap_stats;

static void jmem_heap_stat_init (void);
static void jmem_heap_stat_alloc (size_t num);
static void jmem_heap_stat_free (size_t num);
static void jmem_heap_stat_skip ();
static void jmem_heap_stat_nonskip ();
static void jmem_heap_stat_alloc_iter ();
static void jmem_heap_stat_free_iter ();

#  define JMEM_HEAP_STAT_INIT() jmem_heap_stat_init ()
#  define JMEM_HEAP_STAT_ALLOC(v1) jmem_heap_stat_alloc (v1)
#  define JMEM_HEAP_STAT_FREE(v1) jmem_heap_stat_free (v1)
#  define JMEM_HEAP_STAT_SKIP() jmem_heap_stat_skip ()
#  define JMEM_HEAP_STAT_NONSKIP() jmem_heap_stat_nonskip ()
#  define JMEM_HEAP_STAT_ALLOC_ITER() jmem_heap_stat_alloc_iter ()
#  define JMEM_HEAP_STAT_FREE_ITER() jmem_heap_stat_free_iter ()
#else /* !JMEM_STATS */
#  define JMEM_HEAP_STAT_INIT()
#  define JMEM_HEAP_STAT_ALLOC(v1)
#  define JMEM_HEAP_STAT_FREE(v1)
#  define JMEM_HEAP_STAT_SKIP()
#  define JMEM_HEAP_STAT_NONSKIP()
#  define JMEM_HEAP_STAT_ALLOC_ITER()
#  define JMEM_HEAP_STAT_FREE_ITER()
#endif /* JMEM_STATS */

/**
 * Startup initialization of heap
 */
void
jmem_heap_init (void)
{
  JERRY_ASSERT ((uintptr_t) jmem_heap.area % JMEM_ALIGNMENT == 0);

  JERRY_STATIC_ASSERT ((1u << JMEM_HEAP_OFFSET_LOG) >= JMEM_HEAP_SIZE,
                       two_pow_mem_heap_offset_should_not_be_less_than_mem_heap_size);

  jmem_heap_allocated_size = 0;
  jmem_heap_limit = CONFIG_MEM_HEAP_DESIRED_LIMIT;
  jmem_heap.first.size = 0;
  jmem_heap_free_t *const region_p = (jmem_heap_free_t *) jmem_heap.area;
  jmem_heap.first.next_offset = JMEM_HEAP_GET_OFFSET_FROM_ADDR (region_p);
  region_p->size = sizeof (jmem_heap.area);
  region_p->next_offset = JMEM_HEAP_GET_OFFSET_FROM_ADDR (JMEM_HEAP_END_OF_LIST);

  jmem_heap_list_skip_p = &jmem_heap.first;

  VALGRIND_NOACCESS_SPACE (jmem_heap.area, JMEM_HEAP_AREA_SIZE);

  JMEM_HEAP_STAT_INIT ();
} /* jmem_heap_init */

/**
 * Finalize heap
 */
void jmem_heap_finalize (void)
{
  JERRY_ASSERT (jmem_heap_allocated_size == 0);
  VALGRIND_NOACCESS_SPACE (&jmem_heap, sizeof (jmem_heap));
} /* jmem_heap_finalize */

/**
 * Allocation of memory region.
 *
 * See also:
 *          jmem_heap_alloc_block
 *
 * @return pointer to allocated memory block - if allocation is successful,
 *         NULL - if there is not enough memory.
 */
static __attribute__((hot))
void *jmem_heap_alloc_block_internal (const size_t size)
{
  // Align size
  const size_t required_size = ((size + JMEM_ALIGNMENT - 1) / JMEM_ALIGNMENT) * JMEM_ALIGNMENT;
  jmem_heap_free_t *data_space_p = NULL;

  VALGRIND_DEFINED_SPACE (&jmem_heap.first, sizeof (jmem_heap_free_t));

  // Fast path for 8 byte chunks, first region is guaranteed to be sufficient
  if (required_size == JMEM_ALIGNMENT
      && likely (jmem_heap.first.next_offset != JMEM_HEAP_GET_OFFSET_FROM_ADDR (JMEM_HEAP_END_OF_LIST)))
  {
    data_space_p = JMEM_HEAP_GET_ADDR_FROM_OFFSET (jmem_heap.first.next_offset);
    JERRY_ASSERT (jmem_is_heap_pointer (data_space_p));

    VALGRIND_DEFINED_SPACE (data_space_p, sizeof (jmem_heap_free_t));
    jmem_heap_allocated_size += JMEM_ALIGNMENT;
    JMEM_HEAP_STAT_ALLOC_ITER ();

    if (data_space_p->size == JMEM_ALIGNMENT)
    {
      jmem_heap.first.next_offset = data_space_p->next_offset;
    }
    else
    {
      JERRY_ASSERT (data_space_p->size > JMEM_ALIGNMENT);
      jmem_heap_free_t *const remaining_p = JMEM_HEAP_GET_ADDR_FROM_OFFSET (jmem_heap.first.next_offset) + 1;

      VALGRIND_DEFINED_SPACE (remaining_p, sizeof (jmem_heap_free_t));
      remaining_p->size = data_space_p->size - JMEM_ALIGNMENT;
      remaining_p->next_offset = data_space_p->next_offset;
      VALGRIND_NOACCESS_SPACE (remaining_p, sizeof (jmem_heap_free_t));

      jmem_heap.first.next_offset = JMEM_HEAP_GET_OFFSET_FROM_ADDR (remaining_p);
    }

    VALGRIND_UNDEFINED_SPACE (data_space_p, sizeof (jmem_heap_free_t));

    if (unlikely (data_space_p == jmem_heap_list_skip_p))
    {
      jmem_heap_list_skip_p = JMEM_HEAP_GET_ADDR_FROM_OFFSET (jmem_heap.first.next_offset);
    }
  }
  // Slow path for larger regions
  else
  {
    jmem_heap_free_t *current_p = JMEM_HEAP_GET_ADDR_FROM_OFFSET (jmem_heap.first.next_offset);
    jmem_heap_free_t *prev_p = &jmem_heap.first;
    while (current_p != JMEM_HEAP_END_OF_LIST)
    {
      JERRY_ASSERT (jmem_is_heap_pointer (current_p));
      VALGRIND_DEFINED_SPACE (current_p, sizeof (jmem_heap_free_t));
      JMEM_HEAP_STAT_ALLOC_ITER ();

      const uint32_t next_offset = current_p->next_offset;
      JERRY_ASSERT (jmem_is_heap_pointer (JMEM_HEAP_GET_ADDR_FROM_OFFSET (next_offset))
                    || next_offset == JMEM_HEAP_GET_OFFSET_FROM_ADDR (JMEM_HEAP_END_OF_LIST));

      if (current_p->size >= required_size)
      {
        // Region is sufficiently big, store address
        data_space_p = current_p;
        jmem_heap_allocated_size += required_size;

        // Region was larger than necessary
        if (current_p->size > required_size)
        {
          // Get address of remaining space
          jmem_heap_free_t *const remaining_p = (jmem_heap_free_t *) ((uint8_t *) current_p + required_size);

          // Update metadata
          VALGRIND_DEFINED_SPACE (remaining_p, sizeof (jmem_heap_free_t));
          remaining_p->size = current_p->size - (uint32_t) required_size;
          remaining_p->next_offset = next_offset;
          VALGRIND_NOACCESS_SPACE (remaining_p, sizeof (jmem_heap_free_t));

          // Update list
          VALGRIND_DEFINED_SPACE (prev_p, sizeof (jmem_heap_free_t));
          prev_p->next_offset = JMEM_HEAP_GET_OFFSET_FROM_ADDR (remaining_p);
          VALGRIND_NOACCESS_SPACE (prev_p, sizeof (jmem_heap_free_t));
        }
        // Block is an exact fit
        else
        {
          // Remove the region from the list
          VALGRIND_DEFINED_SPACE (prev_p, sizeof (jmem_heap_free_t));
          prev_p->next_offset = next_offset;
          VALGRIND_NOACCESS_SPACE (prev_p, sizeof (jmem_heap_free_t));
        }

        jmem_heap_list_skip_p = prev_p;

        // Found enough space
        break;
      }

      VALGRIND_NOACCESS_SPACE (current_p, sizeof (jmem_heap_free_t));
      // Next in list
      prev_p = current_p;
      current_p = JMEM_HEAP_GET_ADDR_FROM_OFFSET (next_offset);
    }
  }

  while (jmem_heap_allocated_size >= jmem_heap_limit)
  {
    jmem_heap_limit += CONFIG_MEM_HEAP_DESIRED_LIMIT;
  }

  VALGRIND_NOACCESS_SPACE (&jmem_heap.first, sizeof (jmem_heap_free_t));

  if (unlikely (!data_space_p))
  {
    return NULL;
  }

  JERRY_ASSERT ((uintptr_t) data_space_p % JMEM_ALIGNMENT == 0);
  VALGRIND_UNDEFINED_SPACE (data_space_p, size);
  JMEM_HEAP_STAT_ALLOC (size);

  return (void *) data_space_p;
} /* jmem_heap_finalize */

/**
 * Allocation of memory block, running 'try to give memory back' callbacks, if there is not enough memory.
 *
 * Note:
 *      if after running the callbacks, there is still not enough memory, NULL value will be returned
 *
 * @return pointer to allocated memory block or NULL in case of unsuccessful allocation
 */
static void *
jmem_heap_gc_and_alloc_block (const size_t size) /**< required memory size */
{
  VALGRIND_FREYA_CHECK_MEMPOOL_REQUEST;

#ifdef JMEM_GC_BEFORE_EACH_ALLOC
  jmem_run_free_unused_memory_callbacks (JMEM_FREE_UNUSED_MEMORY_SEVERITY_HIGH);
#endif /* JMEM_GC_BEFORE_EACH_ALLOC */

  if (jmem_heap_allocated_size + size >= jmem_heap_limit)
  {
    jmem_run_free_unused_memory_callbacks (JMEM_FREE_UNUSED_MEMORY_SEVERITY_LOW);
  }

  void *data_space_p = jmem_heap_alloc_block_internal (size);

  if (likely (data_space_p != NULL))
  {
    VALGRIND_FREYA_MALLOCLIKE_SPACE (data_space_p, size);
    return data_space_p;
  }

  for (jmem_free_unused_memory_severity_t severity = JMEM_FREE_UNUSED_MEMORY_SEVERITY_LOW;
       severity <= JMEM_FREE_UNUSED_MEMORY_SEVERITY_HIGH;
       severity = (jmem_free_unused_memory_severity_t) (severity + 1))
  {
    jmem_run_free_unused_memory_callbacks (severity);

    data_space_p = jmem_heap_alloc_block_internal (size);

    if (likely (data_space_p != NULL))
    {
      VALGRIND_FREYA_MALLOCLIKE_SPACE (data_space_p, size);
      return data_space_p;
    }
  }

  JERRY_ASSERT (data_space_p == NULL);
  return data_space_p;
} /* jmem_heap_gc_and_alloc_block */


/**
 * Allocation of memory block, running 'try to give memory back' callbacks, if there is not enough memory.
 *
 * Note:
 *      If there is still not enough memory after running the callbacks, then the engine will be
 *      terminated with ERR_OUT_OF_MEMORY.
 *
 * @return NULL, if the required memory is 0
 *         pointer to allocated memory block, otherwise
 */
void * __attribute__((hot))
jmem_heap_alloc_block (const size_t size)  /**< required memory size */
{
  if (unlikely (size == 0))
  {
    return NULL;
  }

  void *data_space_p = jmem_heap_gc_and_alloc_block (size);

  if (likely (data_space_p != NULL))
  {
    return data_space_p;
  }

  jerry_fatal (ERR_OUT_OF_MEMORY);
} /* jmem_heap_alloc_block */

/**
 * Allocation of memory block, running 'try to give memory back' callbacks, if there is not enough memory.
 *
 * Note:
 *      If there is still not enough memory after running the callbacks, NULL will be returned.
 *
 * @return NULL, if the required memory size is 0
 *         also NULL, if the allocation has failed
 *         pointer to allocated memory block, otherwise
 */
void * __attribute__((hot))
jmem_heap_alloc_block_null_on_error (const size_t size) /**< required memory size */
{
  if (unlikely (size == 0))
  {
    return NULL;
  }

  return jmem_heap_gc_and_alloc_block (size);
} /* jmem_heap_alloc_block_null_on_error */


/**
 *  Allocate block and store block size.
 *
 * Note: block will only be aligned to 4 bytes.
 */
inline void * __attr_always_inline___
jmem_heap_alloc_block_store_size (size_t size) /**< required size */
{
  if (unlikely (size == 0))
  {
    return NULL;
  }

  size += sizeof (jmem_heap_free_t);

  jmem_heap_free_t *const data_space_p = (jmem_heap_free_t *) jmem_heap_alloc_block (size);
  data_space_p->size = (uint32_t) size;
  return (void *) (data_space_p + 1);
} /* jmem_heap_alloc_block_store_size */

/**
 * Free the memory block.
 */
void __attribute__((hot))
jmem_heap_free_block (void *ptr, /**< pointer to beginning of data space of the block */
                      const size_t size) /**< size of allocated region */
{
  VALGRIND_FREYA_CHECK_MEMPOOL_REQUEST;

  /* checking that ptr points to the heap */
  JERRY_ASSERT (jmem_is_heap_pointer (ptr));
  JERRY_ASSERT (size > 0);
  JERRY_ASSERT (jmem_heap_limit >= jmem_heap_allocated_size);

  VALGRIND_FREYA_FREELIKE_SPACE (ptr);
  VALGRIND_NOACCESS_SPACE (ptr, size);
  JMEM_HEAP_STAT_FREE_ITER ();

  jmem_heap_free_t *block_p = (jmem_heap_free_t *) ptr;
  jmem_heap_free_t *prev_p;
  jmem_heap_free_t *next_p;

  VALGRIND_DEFINED_SPACE (&jmem_heap.first, sizeof (jmem_heap_free_t));

  if (block_p > jmem_heap_list_skip_p)
  {
    prev_p = jmem_heap_list_skip_p;
    JMEM_HEAP_STAT_SKIP ();
  }
  else
  {
    prev_p = &jmem_heap.first;
    JMEM_HEAP_STAT_NONSKIP ();
  }

  JERRY_ASSERT (jmem_is_heap_pointer (block_p));
  const uint32_t block_offset = JMEM_HEAP_GET_OFFSET_FROM_ADDR (block_p);

  VALGRIND_DEFINED_SPACE (prev_p, sizeof (jmem_heap_free_t));
  // Find position of region in the list
  while (prev_p->next_offset < block_offset)
  {
    jmem_heap_free_t *const next_p = JMEM_HEAP_GET_ADDR_FROM_OFFSET (prev_p->next_offset);
    JERRY_ASSERT (jmem_is_heap_pointer (next_p));

    VALGRIND_DEFINED_SPACE (next_p, sizeof (jmem_heap_free_t));
    VALGRIND_NOACCESS_SPACE (prev_p, sizeof (jmem_heap_free_t));
    prev_p = next_p;

    JMEM_HEAP_STAT_FREE_ITER ();
  }

  next_p = JMEM_HEAP_GET_ADDR_FROM_OFFSET (prev_p->next_offset);
  VALGRIND_DEFINED_SPACE (next_p, sizeof (jmem_heap_free_t));

  /* Realign size */
  const size_t aligned_size = (size + JMEM_ALIGNMENT - 1) / JMEM_ALIGNMENT * JMEM_ALIGNMENT;

  VALGRIND_DEFINED_SPACE (block_p, sizeof (jmem_heap_free_t));
  VALGRIND_DEFINED_SPACE (prev_p, sizeof (jmem_heap_free_t));
  // Update prev
  if (jmem_heap_get_region_end (prev_p) == block_p)
  {
    // Can be merged
    prev_p->size += (uint32_t) aligned_size;
    VALGRIND_NOACCESS_SPACE (block_p, sizeof (jmem_heap_free_t));
    block_p = prev_p;
  }
  else
  {
    block_p->size = (uint32_t) aligned_size;
    prev_p->next_offset = block_offset;
  }

  VALGRIND_DEFINED_SPACE (next_p, sizeof (jmem_heap_free_t));
  // Update next
  if (jmem_heap_get_region_end (block_p) == next_p)
  {
    if (unlikely (next_p == jmem_heap_list_skip_p))
    {
      jmem_heap_list_skip_p = block_p;
    }

    // Can be merged
    block_p->size += next_p->size;
    block_p->next_offset = next_p->next_offset;

  }
  else
  {
    block_p->next_offset = JMEM_HEAP_GET_OFFSET_FROM_ADDR (next_p);
  }

  jmem_heap_list_skip_p = prev_p;

  VALGRIND_NOACCESS_SPACE (prev_p, sizeof (jmem_heap_free_t));
  VALGRIND_NOACCESS_SPACE (block_p, size);
  VALGRIND_NOACCESS_SPACE (next_p, sizeof (jmem_heap_free_t));

  JERRY_ASSERT (jmem_heap_allocated_size > 0);
  jmem_heap_allocated_size -= aligned_size;

  while (jmem_heap_allocated_size + CONFIG_MEM_HEAP_DESIRED_LIMIT <= jmem_heap_limit)
  {
    jmem_heap_limit -= CONFIG_MEM_HEAP_DESIRED_LIMIT;
  }

  VALGRIND_NOACCESS_SPACE (&jmem_heap.first, sizeof (jmem_heap_free_t));
  JERRY_ASSERT (jmem_heap_limit >= jmem_heap_allocated_size);
  JMEM_HEAP_STAT_FREE (size);
} /* jmem_heap_free_block */

/**
 * Free block with stored size
 */
inline void __attr_always_inline___
jmem_heap_free_block_size_stored (void *ptr) /**< pointer to the memory block */
{
  jmem_heap_free_t *const original_p = ((jmem_heap_free_t *) ptr) - 1;
  JERRY_ASSERT (original_p + 1 == ptr);
  jmem_heap_free_block (original_p, original_p->size);
} /* jmem_heap_free_block_size_stored */

/**
 * Compress pointer
 *
 * @return packed heap pointer
 */
uintptr_t __attr_pure___ __attribute__((hot))
jmem_heap_compress_pointer (const void *pointer_p) /**< pointer to compress */
{
  JERRY_ASSERT (pointer_p != NULL);
  JERRY_ASSERT (jmem_is_heap_pointer (pointer_p));

  uintptr_t int_ptr = (uintptr_t) pointer_p;
  const uintptr_t heap_start = (uintptr_t) &jmem_heap;

  JERRY_ASSERT (int_ptr % JMEM_ALIGNMENT == 0);

  int_ptr -= heap_start;
  int_ptr >>= JMEM_ALIGNMENT_LOG;

  JERRY_ASSERT ((int_ptr & ~((1u << JMEM_HEAP_OFFSET_LOG) - 1)) == 0);

  JERRY_ASSERT (int_ptr != JMEM_CP_NULL);

  return int_ptr;
} /* jmem_heap_compress_pointer */

/**
 * Decompress pointer
 *
 * @return unpacked heap pointer
 */
void *  __attr_pure___ __attribute__((hot))
jmem_heap_decompress_pointer (uintptr_t compressed_pointer) /**< pointer to decompress */
{
  JERRY_ASSERT (compressed_pointer != JMEM_CP_NULL);

  uintptr_t int_ptr = compressed_pointer;
  const uintptr_t heap_start = (uintptr_t) &jmem_heap;

  int_ptr <<= JMEM_ALIGNMENT_LOG;
  int_ptr += heap_start;

  JERRY_ASSERT (jmem_is_heap_pointer ((void *) int_ptr));
  return (void *) int_ptr;
} /* jmem_heap_decompress_pointer */

#ifndef JERRY_NDEBUG
/**
 * Check whether the pointer points to the heap
 *
 * Note:
 *      the routine should be used only for assertion checks
 *
 * @return true - if pointer points to the heap,
 *         false - otherwise
 */
bool
jmem_is_heap_pointer (const void *pointer) /**< pointer */
{
  return ((uint8_t *) pointer >= jmem_heap.area
          && (uint8_t *) pointer <= ((uint8_t *) jmem_heap.area + JMEM_HEAP_AREA_SIZE));
} /* jmem_is_heap_pointer */
#endif /* !JERRY_NDEBUG */

#ifdef JMEM_STATS
/**
 * Get heap memory usage statistics
 */
void
jmem_heap_get_stats (jmem_heap_stats_t *out_heap_stats_p) /**< [out] heap stats */
{
  JERRY_ASSERT (out_heap_stats_p != NULL);

  *out_heap_stats_p = jmem_heap_stats;
} /* jmem_heap_get_stats */

/**
 * Reset peak values in memory usage statistics
 */
void
jmem_heap_stats_reset_peak (void)
{
  jmem_heap_stats.peak_allocated_bytes = jmem_heap_stats.allocated_bytes;
  jmem_heap_stats.peak_waste_bytes = jmem_heap_stats.waste_bytes;
} /* jmem_heap_stats_reset_peak */

/**
 * Print heap memory usage statistics
 */
void
jmem_heap_stats_print (void)
{
  printf ("Heap stats:\n"
          "  Heap size = %zu bytes\n"
          "  Allocated = %zu bytes\n"
          "  Waste = %zu bytes\n"
          "  Peak allocated = %zu bytes\n"
          "  Peak waste = %zu bytes\n"
          "  Skip-ahead ratio = %zu.%04zu\n"
          "  Average alloc iteration = %zu.%04zu\n"
          "  Average free iteration = %zu.%04zu\n"
          "\n",
          jmem_heap_stats.size,
          jmem_heap_stats.allocated_bytes,
          jmem_heap_stats.waste_bytes,
          jmem_heap_stats.peak_allocated_bytes,
          jmem_heap_stats.peak_waste_bytes,
          jmem_heap_stats.skip_count / jmem_heap_stats.nonskip_count,
          jmem_heap_stats.skip_count % jmem_heap_stats.nonskip_count * 10000 / jmem_heap_stats.nonskip_count,
          jmem_heap_stats.alloc_iter_count / jmem_heap_stats.alloc_count,
          jmem_heap_stats.alloc_iter_count % jmem_heap_stats.alloc_count * 10000 / jmem_heap_stats.alloc_count,
          jmem_heap_stats.free_iter_count / jmem_heap_stats.free_count,
          jmem_heap_stats.free_iter_count % jmem_heap_stats.free_count * 10000 / jmem_heap_stats.free_count);
} /* jmem_heap_stats_print */

/**
 * Initalize heap memory usage statistics account structure
 */
static void
jmem_heap_stat_init ()
{
  memset (&jmem_heap_stats, 0, sizeof (jmem_heap_stats));

  jmem_heap_stats.size = JMEM_HEAP_AREA_SIZE;
} /* jmem_heap_stat_init */

/**
 * Account allocation
 */
static void
jmem_heap_stat_alloc (size_t size) /**< Size of allocated block */
{
  const size_t aligned_size = (size + JMEM_ALIGNMENT - 1) / JMEM_ALIGNMENT * JMEM_ALIGNMENT;
  const size_t waste_bytes = aligned_size - size;

  jmem_heap_stats.allocated_bytes += aligned_size;
  jmem_heap_stats.waste_bytes += waste_bytes;
  jmem_heap_stats.alloc_count++;


  if (jmem_heap_stats.allocated_bytes > jmem_heap_stats.peak_allocated_bytes)
  {
    jmem_heap_stats.peak_allocated_bytes = jmem_heap_stats.allocated_bytes;
  }
  if (jmem_heap_stats.allocated_bytes > jmem_heap_stats.global_peak_allocated_bytes)
  {
    jmem_heap_stats.global_peak_allocated_bytes = jmem_heap_stats.allocated_bytes;
  }

  if (jmem_heap_stats.waste_bytes > jmem_heap_stats.peak_waste_bytes)
  {
    jmem_heap_stats.peak_waste_bytes = jmem_heap_stats.waste_bytes;
  }
  if (jmem_heap_stats.waste_bytes > jmem_heap_stats.global_peak_waste_bytes)
  {
    jmem_heap_stats.global_peak_waste_bytes = jmem_heap_stats.waste_bytes;
  }
} /* jmem_heap_stat_alloc */

/**
 * Account freeing
 */
static void
jmem_heap_stat_free (size_t size) /**< Size of freed block */
{
  const size_t aligned_size = (size + JMEM_ALIGNMENT - 1) / JMEM_ALIGNMENT * JMEM_ALIGNMENT;
  const size_t waste_bytes = aligned_size - size;

  jmem_heap_stats.free_count++;
  jmem_heap_stats.allocated_bytes -= aligned_size;
  jmem_heap_stats.waste_bytes -= waste_bytes;
} /* jmem_heap_stat_free */

/**
 * Counts number of skip-aheads during insertion of free block
 */
static void
jmem_heap_stat_skip ()
{
  jmem_heap_stats.skip_count++;
} /* jmem_heap_stat_skip  */

/**
 * Counts number of times we could not skip ahead during free block insertion
 */
static void
jmem_heap_stat_nonskip ()
{
  jmem_heap_stats.nonskip_count++;
} /* jmem_heap_stat_nonskip */

/**
 * Count number of iterations required for allocations
 */
static void
jmem_heap_stat_alloc_iter ()
{
  jmem_heap_stats.alloc_iter_count++;
} /* jmem_heap_stat_alloc_iter */

/**
 * Counts number of iterations required for inserting free blocks
 */
static void
jmem_heap_stat_free_iter ()
{
  jmem_heap_stats.free_iter_count++;
} /* jmem_heap_stat_free_iter */
#endif /* JMEM_STATS */

/**
 * @}
 * @}
 */
