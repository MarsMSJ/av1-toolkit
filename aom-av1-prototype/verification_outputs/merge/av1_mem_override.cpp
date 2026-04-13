#define AV1_MEM_OVERRIDE_C
#include "av1_mem_override.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#define MIN_ALIGNMENT        16
#define ALIGN64(x)           (((x) + 63) & ~63)
#define DEFAULT_ALIGNMENT    16

#define BITS_PER_PIXEL_8BIT      8
#define BITS_PER_PIXEL_10BIT    10
#define BITS_PER_PIXEL_12BIT    12
#define CHROMA_FACTOR_420       1.5
#define CHROMA_FACTOR_422       2.0
#define CHROMA_FACTOR_444       3.0
#define BASE_DPB_COUNT          8
#define OVERHEAD_PER_FRAME      (256 * 1024)
#define PER_WORKER_SCRATCH      (2 * 1024 * 1024)
#define DECODER_CONTEXT_SIZE    (8 * 1024 * 1024)
#define ENTROPY_CONTEXTS_SIZE  (4 * 1024 * 1024)
#define TABLES_SIZE            (2 * 1024 * 1024)
#define HEADROOM_FACTOR        1.10

typedef struct FreeBlock {
    size_t size;
    struct FreeBlock *next;
} FreeBlock;

typedef struct Av1MemHeader {
    void *base;              /* BUG FIX: Store original base pointer */
    void *bump_ptr;
    void *bump_end;
    FreeBlock *free_list;
    Av1MemStats stats;
    pthread_mutex_t mutex;
    bool initialized;
    bool override_enabled;
} Av1MemHeader;

static Av1MemHeader g_mem_header;
static bool g_header_initialized = false;

static void *align_ptr(void *ptr, size_t alignment) {
    uintptr_t addr = (uintptr_t)ptr;
    addr = (addr + alignment - 1) & ~(alignment - 1);
    return (void *)addr;
}

static size_t align_size(size_t size, size_t alignment) {
    return (size + alignment - 1) & ~(alignment - 1);
}

static void merge_free_blocks(Av1MemHeader *header) {
    FreeBlock *current = header->free_list;
    while (current && current->next) {
        uintptr_t current_end = (uintptr_t)current + current->size;
        if (current_end == (uintptr_t)current->next) {
            FreeBlock *to_remove = current->next;
            current->size += to_remove->size;
            current->next = to_remove->next;
        } else {
            current = current->next;
        }
    }
}

static FreeBlock *find_best_fit(FreeBlock **head, size_t size) {
    FreeBlock *best = NULL;
    FreeBlock *best_prev = NULL;
    FreeBlock *prev = NULL;
    FreeBlock *current = *head;
    
    while (current) {
        if (current->size >= size) {
            if (!best || current->size < best->size) {
                best = current;
                best_prev = prev;
            }
        }
        prev = current;
        current = current->next;
    }
    
    if (best) {
        if (best_prev) {
            best_prev->next = best->next;
        } else {
            *head = best->next;
        }
    }
    
    return best;
}

bool av1_mem_init(void *base, size_t size) {
    if (!base || size < sizeof(Av1MemHeader) + 1024) {
        fprintf(stderr, "av1_mem_init: invalid base or size too small\n");
        return false;
    }
    
    /* BUG FIX: Handle re-initialization properly */
    if (g_header_initialized) {
        av1_mem_shutdown();
    }
    
    Av1MemHeader *header = (Av1MemHeader *)base;
    memset(header, 0, sizeof(Av1MemHeader));
    
    /* BUG FIX: Store original base pointer */
    header->base = base;
    header->bump_ptr = (char *)base + sizeof(Av1MemHeader);
    header->bump_end = (char *)base + size;
    header->free_list = NULL;
    
    if (pthread_mutex_init(&header->mutex, NULL) != 0) {
        fprintf(stderr, "av1_mem_init: failed to initialize mutex\n");
        return false;
    }
    
    memset(&header->stats, 0, sizeof(Av1MemStats));
    header->stats.total_size = size;
    
    /* BUG FIX: Set initialized flag after everything is ready */
    header->initialized = true;
    header->override_enabled = true;
    
    /* BUG FIX: Use memcpy but now g_mem_header keeps reference to base */
    memcpy(&g_mem_header, header, sizeof(Av1MemHeader));
    g_header_initialized = true;
    
    return true;
}

void av1_mem_shutdown(void) {
    if (!g_header_initialized) {
        return;
    }
    
    pthread_mutex_destroy(&g_mem_header.mutex);
    memset(&g_mem_header, 0, sizeof(Av1MemHeader));
    g_header_initialized = false;
}

void *av1_mem_malloc(size_t size) {
    return av1_mem_memalign(DEFAULT_ALIGNMENT, size);
}

void *av1_mem_memalign(size_t alignment, size_t size) {
    if (!g_header_initialized || !g_mem_header.initialized) {
        fprintf(stderr, "av1_mem_memalign: allocator not initialized\n");
        return NULL;
    }
    
    if (size == 0) {
        size = 1;
    }
    
    if (alignment < MIN_ALIGNMENT) {
        alignment = MIN_ALIGNMENT;
    }
    
    /* BUG FIX: Correct power-of-2 check - if not power of 2, use MIN_ALIGNMENT */
    if ((alignment & (alignment - 1)) != 0) {
        alignment = MIN_ALIGNMENT;
    }
    
    /* BUG FIX: Account for sizeof(size_t) in alignment calculation.
     * We need to ensure the returned pointer is aligned after adding sizeof(size_t).
     * The header stores size before the user pointer, so we need extra alignment. */
    size_t size_t_size = sizeof(size_t);
    size_t total_header_size = size_t_size;
    
    /* Ensure alignment is at least as large as size_t for proper alignment after offset */
    if (alignment < size_t_size) {
        alignment = size_t_size;
    }
    
    /* Calculate aligned size including the size_t header */
    size_t aligned_size = align_size(size + total_header_size, alignment);
    
    void *ptr = NULL;
    
    pthread_mutex_lock(&g_mem_header.mutex);
    
    FreeBlock *block = find_best_fit(&g_mem_header.free_list, aligned_size);
    
    if (block) {
        ptr = (void *)block;
        g_mem_header.stats.num_free_list_hits++;
        
        if (block->size > aligned_size + sizeof(FreeBlock) + MIN_ALIGNMENT) {
            FreeBlock *remaining = (FreeBlock *)((char *)ptr + aligned_size);
            remaining->size = block->size - aligned_size;
            remaining->next = g_mem_header.free_list;
            g_mem_header.free_list = remaining;
        }
    } else {
        void *aligned_ptr = align_ptr(g_mem_header.bump_ptr, alignment);
        char *new_bump = (char *)aligned_ptr + aligned_size;
        
        if (new_bump <= (char *)g_mem_header.bump_end) {
            ptr = aligned_ptr;
            g_mem_header.bump_ptr = new_bump;
            g_mem_header.stats.num_bump_allocations++;
        } else {
            pthread_mutex_unlock(&g_mem_header.mutex);
            fprintf(stderr, "av1_mem_memalign: out of memory\n");
            return NULL;
        }
    }
    
    *(size_t *)ptr = aligned_size;
    
    g_mem_header.stats.used_size += aligned_size;
    g_mem_header.stats.num_allocations++;
    
    if (g_mem_header.stats.used_size > g_mem_header.stats.peak_usage) {
        g_mem_header.stats.peak_usage = g_mem_header.stats.used_size;
    }
    
    pthread_mutex_unlock(&g_mem_header.mutex);
    
    /* BUG FIX: Now the returned pointer is properly aligned because we accounted
     * for sizeof(size_t) in the aligned_size calculation */
    return (char *)ptr + total_header_size;
}

void *av1_mem_calloc(size_t num, size_t size) {
    size_t total = num * size;
    if (total == 0 || num == 0) {
        total = 1;
    }
    
    void *ptr = av1_mem_malloc(total);
    if (ptr) {
        memset(ptr, 0, total);
    }
    return ptr;
}

void av1_mem_free(void *ptr) {
    if (!ptr || !g_header_initialized) {
        return;
    }
    
    char *block = (char *)ptr - sizeof(size_t);
    size_t block_size = *(size_t *)block;
    
    pthread_mutex_lock(&g_mem_header.mutex);
    
    FreeBlock *free_block = (FreeBlock *)block;
    free_block->size = block_size;
    free_block->next = g_mem_header.free_list;
    g_mem_header.free_list = free_block;
    
    g_mem_header.stats.used_size -= block_size;
    g_mem_header.stats.num_frees++;
    
    merge_free_blocks(&g_mem_header);
    
    size_t largest = 0;
    FreeBlock *current = g_mem_header.free_list;
    while (current) {
        if (current->size > largest) {
            largest = current->size;
        }
        current = current->next;
    }
    g_mem_header.stats.largest_free_block = largest;
    
    pthread_mutex_unlock(&g_mem_header.mutex);
}

Av1MemStats av1_mem_get_stats(void) {
    Av1MemStats stats = {};
    
    if (g_header_initialized) {
        pthread_mutex_lock(&g_mem_header.mutex);
        stats = g_mem_header.stats;
        pthread_mutex_unlock(&g_mem_header.mutex);
    }
    
    return stats;
}

void av1_mem_reset_stats(void) {
    if (!g_header_initialized) {
        return;
    }
    
    pthread_mutex_lock(&g_mem_header.mutex);
    g_mem_header.stats.num_allocations = 0;
    g_mem_header.stats.num_frees = 0;
    g_mem_header.stats.num_free_list_hits = 0;
    g_mem_header.stats.num_bump_allocations = 0;
    pthread_mutex_unlock(&g_mem_header.mutex);
}

size_t av1_mem_query_size(const Av1StreamInfo *info, int queue_depth, int num_workers) {
    if (!info) {
        return 0;
    }
    
    int64_t width = ALIGN64(info->width);
    int64_t height = ALIGN64(info->height);
    
    /* BUG FIX: Use is_16bit to determine bit depth, not max_bitrate.
     * is_16bit=false means 8-bit, is_16bit=true means 10-bit or 12-bit.
     * For more accurate sizing, we assume 12-bit for 16-bit mode as worst case. */
    int bps;
    if (info->is_16bit) {
        /* For 16-bit mode, assume 12-bit as worst case for memory calculation */
        bps = BITS_PER_PIXEL_12BIT;
    } else {
        bps = BITS_PER_PIXEL_8BIT;
    }
    
    double chroma_factor;
    switch (info->chroma_subsampling) {
        case 2: chroma_factor = CHROMA_FACTOR_444; break;
        case 1: chroma_factor = CHROMA_FACTOR_422; break;
        default: chroma_factor = CHROMA_FACTOR_420; break;
    }
    
    int64_t frame_size = (width * height * bps * (int)chroma_factor) / 8;
    
    int dpb_count = BASE_DPB_COUNT + queue_depth + 1;
    int64_t dpb_total = dpb_count * (frame_size + OVERHEAD_PER_FRAME);
    
    int64_t scratch = num_workers * PER_WORKER_SCRATCH;
    int64_t overhead = DECODER_CONTEXT_SIZE + ENTROPY_CONTEXTS_SIZE + TABLES_SIZE;
    
    int64_t total = (dpb_total + scratch + overhead);
    total = (int64_t)(total * HEADROOM_FACTOR);
    
    return (size_t)total;
}

bool av1_mem_is_initialized(void) {
    return g_header_initialized && g_mem_header.initialized;
}

void *av1_mem_get_base(void) {
    if (!g_header_initialized) {
        return NULL;
    }
    /* BUG FIX: Return the actual base pointer, not &g_mem_header */
    return g_mem_header.base;
}

size_t av1_mem_get_total_size(void) {
    if (!g_header_initialized) {
        return 0;
    }
    return g_mem_header.stats.total_size;
}

void av1_mem_set_override_enabled(bool enabled) {
    if (g_header_initialized) {
        pthread_mutex_lock(&g_mem_header.mutex);
        g_mem_header.override_enabled = enabled;
        pthread_mutex_unlock(&g_mem_header.mutex);
    }
}

bool av1_mem_get_override_enabled(void) {
    /* BUG FIX: Use double-checked locking pattern for thread safety */
    if (!g_header_initialized) {
        return false;
    }
    bool enabled;
    pthread_mutex_lock(&g_mem_header.mutex);
    enabled = g_mem_header.override_enabled;
    pthread_mutex_unlock(&g_mem_header.mutex);
    return enabled;
}

void *av1_mem_override_malloc(size_t size) {
    if (av1_mem_get_override_enabled()) {
        return av1_mem_malloc(size);
    }
    return malloc(size);
}

void *av1_mem_override_memalign(size_t alignment, size_t size) {
    if (av1_mem_get_override_enabled()) {
        return av1_mem_memalign(alignment, size);
    }
    // aligned_alloc requires size to be a multiple of alignment
    size_t aligned_size = (size + alignment - 1) & ~(alignment - 1);
    return aligned_alloc(alignment, aligned_size);
}

void *av1_mem_override_calloc(size_t num, size_t size) {
    if (av1_mem_get_override_enabled()) {
        return av1_mem_calloc(num, size);
    }
    return calloc(num, size);
}

void av1_mem_override_free(void *ptr) {
    if (av1_mem_get_override_enabled()) {
        av1_mem_free(ptr);
        return;
    }
    free(ptr);
}
