/*
 * Copyright (C) 2010-2014 Alex Smith
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/**
 * @file
 * @brief               Memory management functions.
 */

#include <lib/list.h>
#include <lib/string.h>
#include <lib/utility.h>

#include <assert.h>
#include <loader.h>
#include <memory.h>

/** Structure representing an area on the heap. */
typedef struct heap_chunk {
    list_t header;                  /**< Link to chunk list. */
    size_t size;                    /**< Size of chunk including struct. */
    bool allocated;                 /**< Whether the chunk is allocated. */
} heap_chunk_t;

/** Size of the heap (128KB). */
#define HEAP_SIZE       131072

/** Statically allocated heap. */
static uint8_t heap[HEAP_SIZE] __aligned(PAGE_SIZE);
static LIST_DECLARE(heap_chunks);

#ifndef TARGET_HAS_MM

/** List of physical memory ranges. */
static LIST_DECLARE(memory_ranges);

#endif /* TARGET_HAS_MM */

/**
 * Heap allocator.
 */

/**
 * Allocate memory from the heap.
 *
 * Allocates temporary memory from the heap. This memory will never reach the
 * kernel. An internal error will be raised if the heap is full.
 *
 * @param size          Size of allocation to make.
 *
 * @return              Address of allocation.
 */
void *malloc(size_t size) {
    heap_chunk_t *chunk = NULL, *new;
    size_t total;

    if (size == 0)
        internal_error("Zero-sized allocation!");

    /* Align all allocations to 8 bytes. */
    size = round_up(size, 8);
    total = size + sizeof(heap_chunk_t);

    /* Create the initial free segment if necessary. */
    if (list_empty(&heap_chunks)) {
        chunk = (heap_chunk_t *)heap;
        chunk->size = HEAP_SIZE;
        chunk->allocated = false;
        list_init(&chunk->header);
        list_append(&heap_chunks, &chunk->header);
    } else {
        /* Search for a free chunk. */
        list_foreach(&heap_chunks, iter) {
            chunk = list_entry(iter, heap_chunk_t, header);
            if (!chunk->allocated && chunk->size >= total) {
                break;
            } else {
                chunk = NULL;
            }
        }

        if (!chunk)
            internal_error("Exhausted heap space (want %zu bytes)", size);
    }

    /* Resize the segment if it is too big. There must be space for a second
     * chunk header afterwards. */
    if (chunk->size >= (total + sizeof(heap_chunk_t))) {
        new = (heap_chunk_t *)((char *)chunk + total);
        new->size = chunk->size - total;
        new->allocated = false;
        list_init(&new->header);
        list_add_after(&chunk->header, &new->header);
        chunk->size = total;
    }

    chunk->allocated = true;
    return ((char *)chunk + sizeof(heap_chunk_t));
}

/** Resize a memory allocation.
 * @param addr          Address of old allocation.
 * @param size          New size of allocation.
 * @return              Address of new allocation, or NULL if size is 0. */
void *realloc(void *addr, size_t size) {
    heap_chunk_t *chunk = NULL;
    void *new;

    if (size == 0) {
        free(addr);
        return NULL;
    } else {
        size = round_up(size, 8);

        if (addr) {
            chunk = (heap_chunk_t *)((char *)addr - sizeof(heap_chunk_t));
            if (chunk->size - sizeof(heap_chunk_t) == size)
                return addr;
        }

        new = malloc(size);

        if (addr) {
            memcpy(new, addr, min(chunk->size - sizeof(heap_chunk_t), size));
            free(addr);
        }

        return new;
    }
}

/** Free memory allocated with free().
 * @param addr          Address of allocation. */
void free(void *addr) {
    heap_chunk_t *chunk, *adj;

    if (!addr)
        return;

    /* Get the chunk and free it. */
    chunk = (heap_chunk_t *)((char *)addr - sizeof(heap_chunk_t));
    if (!chunk->allocated)
        internal_error("Double free on address %p", addr);
    chunk->allocated = false;

    /* Coalesce adjacent free segments. */
    if (chunk != list_last(&heap_chunks, heap_chunk_t, header)) {
        adj = list_next(chunk, header);
        if (!adj->allocated) {
            assert(adj == (heap_chunk_t *)((char *)chunk + chunk->size));
            chunk->size += adj->size;
            list_remove(&adj->header);
        }
    }
    if (chunk != list_first(&heap_chunks, heap_chunk_t, header)) {
        adj = list_prev(chunk, header);
        if (!adj->allocated) {
            assert(chunk == (heap_chunk_t *)((char *)adj + adj->size));
            adj->size += chunk->size;
            list_remove(&chunk->header);
        }
    }
}

/**
 * Physical memory manager.
 */

/** Merge adjacent ranges.
 * @param map           Memory map to add to.
 * @param range         Range to merge. */
static inline void merge_ranges(list_t *maps, memory_range_t *range) {
    memory_range_t *other;
    phys_ptr_t end;

    if (range != list_first(maps, memory_range_t, header)) {
        other = list_prev(range, header);
        end = other->start + other->size;

        if (end == range->start && other->type == range->type) {
            range->start = other->start;
            range->size += other->size;
            list_remove(&other->header);
            free(other);
        }
    }

    if (range != list_last(maps, memory_range_t, header)) {
        other = list_next(range, header);
        end = range->start + range->size;

        if (other->start == end && other->type == range->type) {
            range->size += other->size;
            list_remove(&other->header);
            free(other);
        }
    }
}

/** Add a range of physical memory.
 * @param map           Memory map to add to.
 * @param start         Start of the range (must be page-aligned).
 * @param size          Size of the range (must be page-aligned).
 * @param type          Type of the range. */
void memory_map_insert(list_t *map, phys_ptr_t start, phys_size_t size, uint8_t type) {
    memory_range_t *range, *other, *split;
    phys_ptr_t range_end, other_end;

    assert(!(start % PAGE_SIZE));
    assert(!(size % PAGE_SIZE));
    assert(size);

    range = malloc(sizeof(*range));
    list_init(&range->header);
    range->start = start;
    range->size = size;
    range->type = type;

    range_end = start + size - 1;

    /* Try to find where to insert the region in the list. */
    list_foreach(map, iter) {
        other = list_entry(iter, memory_range_t, header);
        if (start <= other->start) {
            list_add_before(&other->header, &range->header);
            break;
        }
    }

    /* Not before any existing range, goes at the end of the list. */
    if (list_empty(&range->header))
        list_append(map, &range->header);

    /* Check if the new range has overlapped part of the previous range. */
    if (range != list_first(map, memory_range_t, header)) {
        other = list_prev(range, header);
        other_end = other->start + other->size - 1;

        if (range->start <= other_end) {
            if (other_end > range_end) {
                /* Must split the range. */
                split = malloc(sizeof(*split));
                list_init(&split->header);
                split->start = range_end + 1;
                split->size = other_end - range_end;
                split->type = other->type;
                list_add_after(&range->header, &split->header);
            }

            other->size = range->start - other->start;
        }
    }

    /* Swallow up any following ranges that the new range overlaps. */
    list_foreach_safe(&range->header, iter) {
        if (iter == map)
            break;

        other = list_entry(iter, memory_range_t, header);
        other_end = other->start + other->size - 1;

        if (other->start > range_end) {
            break;
        } else if (other_end > range_end) {
            /* Resize the range and finish. */
            other->start = range_end + 1;
            other->size = other_end - range_end;
            break;
        } else {
            /* Completely remove the range. */
            list_remove(&other->header);
            free(other);
        }
    }

    /* Finally, merge the region with adjacent ranges of the same type. */
    merge_ranges(map, range);
}

/** Dump a list of physical memory ranges.
 * @param map           Memory map to dump. */
void memory_map_dump(list_t *map) {
    memory_range_t *range;

    list_foreach(map, iter) {
        range = list_entry(iter, memory_range_t, header);

        dprintf(
            " 0x%016" PRIxPHYS "-0x%016" PRIxPHYS " (%" PRIu64 " KiB) -> ",
            range->start, range->start + range->size, range->size / 1024);

        switch (range->type) {
        case MEMORY_TYPE_FREE:
            dprintf("Free\n");
            break;
        case MEMORY_TYPE_ALLOCATED:
            dprintf("Allocated\n");
            break;
        case MEMORY_TYPE_RECLAIMABLE:
            dprintf("Reclaimable\n");
            break;
        case MEMORY_TYPE_PAGETABLES:
            dprintf("Pagetables\n");
            break;
        case MEMORY_TYPE_STACK:
            dprintf("Stack\n");
            break;
        case MEMORY_TYPE_MODULES:
            dprintf("Modules\n");
            break;
        case MEMORY_TYPE_INTERNAL:
            dprintf("Internal\n");
            break;
        default:
            internal_error("Bad memory type %d", range->type);
        }
    }
}

#ifndef TARGET_HAS_MM

/** Check whether a range can satisfy an allocation.
 * @param range         Range to check.
 * @param size          Size of the allocation.
 * @param align         Alignment of the allocation.
 * @param min_addr      Minimum address for the start of the allocated range.
 * @param max_addr      Maximum address of the end of the allocated range.
 * @param flags         Behaviour flags.
 * @param _phys         Where to store address for allocation.
 * @return              Whether the range can satisfy the allocation. */
static bool is_suitable_range(
    memory_range_t *range, phys_size_t size, phys_size_t align,
    phys_ptr_t min_addr, phys_ptr_t max_addr, unsigned flags, phys_ptr_t *_phys)
{
    phys_ptr_t start, match_start, match_end;

    if (range->type != MEMORY_TYPE_FREE)
        return false;

    /* Check if this range contains addresses in the requested range. */
    match_start = max(min_addr, range->start);
    match_end = min(max_addr, range->start + range->size - 1);
    if (match_end <= match_start)
        return false;

    /* Align the base address and check that the range fits. */
    if (flags & MEMORY_ALLOC_HIGH) {
        start = round_down((match_end - size) + 1, align);
        if (start < match_start)
            return false;
    } else {
        start = round_up(match_start, align);
        if ((start + size - 1) > match_end)
            return false;
    }

    *_phys = start;
    return true;
}

/**
 * Allocate a range of physical memory.
 *
 * Allocates a range of physical memory satisfying the specified constraints.
 * Both the physical address allocated and a virtual address mapping the
 * allocated range will be returned. As such, this function always allocates
 * memory that is accessible in the address space that the loader is running
 * in.
 *
 * @param size          Size of the range (multiple of PAGE_SIZE).
 * @param align         Alignment of the range (power of 2, at least PAGE_SIZE).
 * @param min_addr      Minimum address for the start of the allocated range.
 * @param max_addr      Maximum address of the last byte of the allocated range,
 *                      or 0 for no constraint.
 * @param type          Type to give the allocated range (must not be
 *                      MEMORY_TYPE_FREE).
 * @param flags         Behaviour flags.
 * @param _phys         Where to store physical address of allocation.
 *
 * @return              Virtual address of allocation on success, NULL if the
 *                      MEMORY_ALLOC_CAN_FAIL flag is set and insufficient
 *                      memory is available.
 */
void *memory_alloc(
    phys_size_t size, phys_size_t align, phys_ptr_t min_addr, phys_ptr_t max_addr,
    uint8_t type, unsigned flags, phys_ptr_t *_phys)
{
    list_t *iter;

    assert(!(size % PAGE_SIZE));
    assert(!(align % PAGE_SIZE));
    assert(type != MEMORY_TYPE_FREE);

    if (!align)
        align = PAGE_SIZE;

    /* Ensure that all addresses allocated are accessible to us, and set a
     * sensible minimum address if no constraint was given. */
    if (!min_addr)
        min_addr = TARGET_PHYS_MIN;
    if (!max_addr || max_addr > TARGET_PHYS_MAX)
        max_addr = TARGET_PHYS_MAX;

    assert((max_addr - min_addr) >= (size - 1));

    /* Find a free range that is large enough to hold the new range. */
    iter = (flags & MEMORY_ALLOC_HIGH) ? memory_ranges.prev : memory_ranges.next;
    while (iter != &memory_ranges) {
        memory_range_t *range = list_entry(iter, memory_range_t, header);
        phys_ptr_t start;

        if (is_suitable_range(range, size, align, min_addr, max_addr, flags, &start)) {
            /* Insert a new range over the top of the allocation. */
            memory_map_insert(&memory_ranges, start, size, type);

            dprintf(
                "memory: allocated 0x%" PRIxPHYS "-0x%" PRIxPHYS " (align: 0x%" PRIxPHYS ", type: %u)\n",
                start, start + size, align, type);

            if (_phys)
                *_phys = start;

            return (void *)phys_to_virt(start);
        }

        iter = (flags & MEMORY_ALLOC_HIGH) ? range->header.prev : range->header.next;
    }

    if (flags & MEMORY_ALLOC_CAN_FAIL) {
        return NULL;
    } else {
        boot_error("Insufficient memory available (allocating %" PRIuPHYS " bytes)", size);
    }
}

/** Free a range of physical memory.
 * @param addr          Virtual address of allocation.
 * @param size          Size of range to free. */
void memory_free(void *addr, phys_size_t size) {
    phys_ptr_t phys = virt_to_phys((ptr_t)addr);

    assert(!(phys % PAGE_SIZE));
    assert(!(size % PAGE_SIZE));

    list_foreach(&memory_ranges, iter) {
        memory_range_t *range = list_entry(iter, memory_range_t, header);

        if (range->type != MEMORY_TYPE_FREE) {
            if (phys >= range->start && (phys + size - 1) <= (range->start + range->size - 1)) {
                memory_map_insert(&memory_ranges, phys, size, MEMORY_TYPE_FREE);
                return;
            }
        }
    }

    internal_error("Bad memory_free address 0x%" PRIxPHYS, phys);
}

/** Add a range of physical memory.
 * @param start         Start of the range (must be page-aligned).
 * @param size          Size of the range (must be page-aligned).
 * @param type          Type of the range. */
void memory_add(phys_ptr_t start, phys_size_t size, uint8_t type) {
    memory_map_insert(&memory_ranges, start, size, type);
}

/**
 * Mark all free areas in a range as internal.
 *
 * Searches through the given range and marks all currently free areas as
 * internal, so that they will not be allocated from by memory_alloc(). They
 * will be made free again when memory_finalize() is called.
 *
 * @param start         Start of the range.
 * @param size          Size of the range.
 */
void memory_protect(phys_ptr_t start, phys_size_t size) {
    phys_ptr_t match_start, match_end, end;
    memory_range_t *range;

    start = round_down(start, PAGE_SIZE);
    end = round_up(start + size, PAGE_SIZE) - 1;

    list_foreach_safe(&memory_ranges, iter) {
        range = list_entry(iter, memory_range_t, header);
        if (range->type != MEMORY_TYPE_FREE)
            continue;

        match_start = max(start, range->start);
        match_end = min(end, range->start + range->size - 1);
        if (match_end <= match_start)
            continue;

        memory_map_insert(&memory_ranges, match_start, match_end - match_start + 1, MEMORY_TYPE_INTERNAL);
    }
}

/** Initialise the memory manager. */
void memory_init(void) {
    phys_ptr_t start, end;

    target_memory_probe();

    /* Mark the boot loader itself as internal so that it gets reclaimed before
     * entering the kernel. */
    start = round_down(virt_to_phys((ptr_t)__start), PAGE_SIZE);
    end = round_up(virt_to_phys((ptr_t)__end), PAGE_SIZE);
    memory_protect(start, end - start);

    dprintf("memory: initial memory map:\n");
    memory_map_dump(&memory_ranges);
}

/**
 * Finalize the memory map.
 *
 * This should be called once all memory allocations have been performed. It
 * marks all internal memory ranges as free and returns the final memory map
 * to be passed to the OS.
 *
 * @param map           Head of list to place the memory map into.
 */
void memory_finalize(list_t *map) {
    memory_range_t *range;

    /* Reclaim all internal memory ranges. */
    list_foreach(&memory_ranges, iter) {
        range = list_entry(iter, memory_range_t, header);

        if (range->type == MEMORY_TYPE_INTERNAL) {
            range->type = MEMORY_TYPE_FREE;
            merge_ranges(&memory_ranges, range);
        }
    }

    list_init(map);
    list_splice_before(map, &memory_ranges);
}

#endif /* TARGET_HAS_MM */
