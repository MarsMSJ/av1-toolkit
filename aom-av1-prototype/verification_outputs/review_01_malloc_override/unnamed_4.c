#define AV1_MEM_OVERRIDE_C
#include "av1_mem_override.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdalign.h>

// ============================================================================
// Constants and Configuration
// ============================================================================

#define MIN_ALIGNMENT        16
#define ALIGN64(x)           (((x) + 63) & ~63)
#define DEFAULT_ALIGNMENT    16

// Memory estimation constants (based on AOM reference decoder)
#define BITS_PER_PIXEL_8BIT      8
#define BITS_PER_PIXEL_10BIT    10
#define BITS_PER_PIXEL_12BIT    12
#define CHROMA_FACTOR_420       1.5   // Y + 0.5*(U+V)
#define CHROMA_FACTOR_422       2.0   // Y + (U+V)
#define CHROMA_FACTOR_444       3.0   // Y + U + V
#define BASE_DPB_COUNT          8
#define OVERHEAD_PER_FRAME      (256 * 1024)  // 256KB per frame overhead
#define PER_WORKER_SCRATCH      (2 * 1024 * 1024)  // 2MB per worker
#define DECODER_CONTEXT_SIZE    (8 * 1024 * 1024)  // 8MB for decoder context
#define ENTROPY_CONTEXTS_SIZE  (4 * 1024 * 1024)  // 4MB for entropy contexts
#define TABLES_SIZE            (2 * 1024 * 1024)  // 2MB for tables
#define HEADROOM_FACTOR        1.10  // 10% headroom

// ============================================================================
// Internal Structures
// ============================================================================

typedef struct FreeBlock {
    size_t size;
    struct FreeBlock *next;
} FreeBlock;

// Metadata stored separately from user allocations
typedef struct Av1MemMetadata {
    size_t size;
} Av1MemMetadata;

typedef struct Av1MemHeader {
    // Bump allocator state
    void *bump_ptr;
    void *bump_end;
    
    // Free list for deallocated blocks
    FreeBlock *free_list;
    
    // Statistics
    Av1MemStats stats;
    
    // Thread safety
    pthread_mutex_t mutex;
    
    // State flags
    bool initialized;
    bool override_enabled;
    
    // Base pointer for bounds checking
    void *base;
    size_t total_size;
} Av1MemHeader;

// ============================================================================
// Global State - Pointer to the header in user-provided memory
// ============================================================================

static Av1MemHeader *g_mem_header = NULL;

// ============================================================================
// Internal Functions
// ============================================================================

static void *align_ptr(void *ptr, size_t alignment) {
    uintptr_t addr = (uintptr_t)ptr;
    addr = (addr + alignment - 1) & ~(alignment - 1);
    return (void *)addr;
}

static size_t align_size(size_t size, size_t alignment) {
    return (size + alignment - 1) & ~(alignment - 1);
}

// Merge adjacent free blocks
static void merge_free_blocks(Av1MemHeader *header) {
    FreeBlock *current = header->free_list;
    
    while (current && current->next) {
        // Check if current and next are adjacent
        uintptr_t current_end = (uintptr_t)current + current->size;
        if (current_end == (uintptr_t)current->next) {
            // Merge them
            FreeBlock *to_remove = current->next;
            current->size += to_remove->size;
            current->next = to_remove->next;
        } else {
            current = current->next;
        }
    }
}

// Find best-fit block in free list
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
        // Remove from free list
        if (best_prev) {
            best_prev->next = best->next;
        } else {
            *head = best->next;
        }
    }
    
    return best;
}

// Check if pointer is within allocator bounds
static bool is_valid_pointer(Av1MemHeader *header, void *ptr) {
    if (!header || !ptr) return false;
    uintptr_t addr = (uintptr_t)ptr;
    uintptr_t base = (uintptr_t)header->base;
    uintptr_t end = base + header->total_size;
    return addr >= base && addr < end;
}

// ============================================================================
// Public API Implementation
// ============================================================================

bool av1_mem_init(void *base, size_t size) {
    if (!base || size < sizeof(Av1MemHeader) + 1024) {
        fprintf(stderr, "av1_mem_init: invalid base or size too small\n");
        return false;
    }
    
    // Initialize header in place
    Av1MemHeader *header = (Av1MemHeader *)base;
    
    memset(header, 0, sizeof(Av1MemHeader));
    
    // Set up bump allocator region (after header)
    header->bump_ptr = (char *)base + sizeof(Av1MemHeader);
    header->bump_end = (char *)base + size;
    
    // Store base and size for bounds checking
    header->base = base;
    header->total_size = size;
    
    // Initialize free list (empty)
    header->free_list = NULL;
    
    // Initialize mutex
    if (pthread_mutex_init(&header->mutex, NULL) != 0) {
        fprintf(stderr, "av1_mem_init: failed to initialize mutex\n");
        return false;
    }
    
    // Initialize statistics
    memset(&header->stats, 0, sizeof(Av1MemStats));
    header->stats.total_size = size;
    
    header->initialized = true;
    header->override_enabled = true;
    
    // Set global pointer to the actual header
    g_mem_header = header;
    
    return true;
}

void av1_mem_shutdown(void) {
    if (!g_mem_header || !g_mem_header->initialized) {
        return;
    }
    
    pthread_mutex_destroy(&g_mem_header->mutex);
    g_mem_header->initialized = false;
    g_mem_header = NULL;
}

void *av1_mem_malloc(size_t size) {
    return av1_mem_memalign(DEFAULT_ALIGNMENT, size);
}

void *av1_mem_memalign(size_t alignment, size_t size) {
    if (!g_mem_header || !g_mem_header->initialized) {
        fprintf(stderr, "av1_mem_memalign: allocator not initialized\n");
        return NULL;
    }
    
    if (size == 0) {
        size = 1;
    }
    
    // Ensure alignment is valid and at least MIN_ALIGNMENT
    if (alignment == 0 || alignment < MIN_ALIGNMENT) {
        alignment = MIN_ALIGNMENT;
    }
    // Round up to next power of 2 if not already a power of 2
    if ((alignment & (alignment - 1)) != 0) {
        // Find next power of 2
        size_t new_align = MIN_ALIGNMENT;
        while (new_align < alignment) {
            new_align <<= 1;
        }
        alignment = new_align;
    }
    
    // Calculate aligned size including space for metadata
    // We need space for metadata AND the user pointer must be aligned
    // Layout: [metadata][user_data] with user_data aligned to requested alignment
    size_t meta_size = sizeof(Av1MemMetadata);
    size_t aligned_size = align_size(size + meta_size, alignment);
    
    void *ptr = NULL;
    
    pthread_mutex_lock(&g_mem_header->mutex);
    
    // First, try to find a suitable block in the free list
    FreeBlock *block = find_best_fit(&g_mem_header->free_list, aligned_size);
    
    if (block) {
        // Use the free block
        ptr = (void *)block;
        g_mem_header->stats.num_free_list_hits++;
        
        // If block is larger than needed, split it
        if (block->size > aligned_size + sizeof(FreeBlock) + MIN_ALIGNMENT) {
            FreeBlock *remaining = (FreeBlock *)((char *)ptr + aligned_size);
            remaining->size = block->size - aligned_size;
            remaining->next = g_mem_header->free_list;
            g_mem_header->free_list = remaining;
        }
    } else {
        // Try bump allocator
        void *aligned_ptr = align_ptr(g_mem_header->bump_ptr, alignment);
        char *new_bump = (char *)aligned_ptr + aligned_size;
        
        if (new_bump <= (char *)g_mem_header->bump_end) {
            ptr = aligned_ptr;
            g_mem_header->bump_ptr = new_bump;
            g_mem_header->stats.num_bump_allocations++;
        } else {
            // Out of memory
            pthread_mutex_unlock(&g_mem_header->mutex);
            fprintf(stderr, "av1_mem_memalign: out of memory\n");
            return NULL;
        }
    }
    
    // Store metadata at the beginning of the allocation
    Av1MemMetadata *meta = (Av1MemMetadata *)ptr;
    meta->size = aligned_size;
    
    // Update statistics
    g_mem_header->stats.used_size += aligned_size;
    g_mem_header->stats.num_allocations++;
    
    if (g_mem_header->stats.used_size > g_mem_header->stats.peak_usage) {
        g_mem_header->stats.peak_usage = g_mem_header->stats.used_size;
    }
    
    pthread_mutex_unlock(&g_mem_header->mutex);
    
    // Return pointer past the metadata, properly aligned
    return (char *)ptr + meta_size;
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
    if (!ptr || !g_mem_header || !g_mem_header->initialized) {
        return;
    }
    
    // Get the actual block pointer (before metadata)
    char *block = (char *)ptr - sizeof(Av1MemMetadata);
    
    // Validate pointer is within our memory region
    pthread_mutex_lock(&g_mem_header->mutex);
    
    if (!is_valid_pointer(g_mem_header, block)) {
        pthread_mutex_unlock(&g_mem_header->mutex);
        fprintf(stderr, "av1_mem_free: invalid pointer %p\n", ptr);
        return;
    }
    
    Av1MemMetadata *meta = (Av1MemMetadata *)block;
    size_t block_size = meta->size;
    
    // Add to free list
    FreeBlock *free_block = (FreeBlock *)block;
    free_block->size = block_size;
    free_block->next = g_mem_header->free_list;
    g_mem_header->free_list = free_block;
    
    // Update statistics
    g_mem_header->stats.used_size -= block_size;
    g_mem_header->stats.num_frees++;
    
    // Try to merge adjacent free blocks
    merge_free_blocks(g_mem_header);
    
    // Update largest free block
    size_t largest = 0;
    FreeBlock *current = g_mem_header->free_list;
    while (current) {
        if (current->size > largest) {
            largest = current->size;
        }
        current = current->next;
    }
    g_mem_header->stats.largest_free_block = largest;
    
    pthread_mutex_unlock(&g_mem_header->mutex);
}

Av1MemStats av1_mem_get_stats(void) {
    Av1MemStats stats = {0};
    
    if (g_mem_header && g_mem_header->initialized) {
        pthread_mutex_lock(&g_mem_header->mutex);
        stats = g_mem_header->stats;
        
        // Calculate fragmentation as ratio of largest free block to total free
        size_t total_free = g_mem_header->stats.total_size - g_mem_header->stats.used_size;
        if (total_free > 0 && stats.largest_free_block > 0) {
            double frag = 1.0 - ((double)stats.largest_free_block / (double)total_free);
            (void)frag;
        }
        
        pthread_mutex_unlock(&g_mem_header->mutex);
    }
    
    return stats;
}

void av1_mem_reset_stats(void) {
    if (!g_mem_header || !g_mem_header->initialized) {
        return;
    }
    
    pthread_mutex_lock(&g_mem_header->mutex);
    g_mem_header->stats.num_allocations = 0;
    g_mem_header->stats.num_frees = 0;
    g_mem_header->stats.num_free_list_hits = 0;
    g_mem_header->stats.num_bump_allocations = 0;
    pthread_mutex_unlock(&g_mem_header->mutex);
}

size_t av1_mem_query_size(const Av1StreamInfo *info, int queue_depth, int num_workers) {
    if (!info) {
        return 0;
    }
    
    // Calculate frame size
    int64_t width = ALIGN64(info->width);
    int64_t height = ALIGN64(info->height);
    int bps = info->max_bitrate > 0 ? info->max_bitrate : BITS_PER_PIXEL_8BIT;
    
    double chroma_factor;
    switch (info->chroma_subsampling) {
        case 2: chroma_factor = CHROMA_FACTOR_444; break;  // 444
        case 1: chroma_factor = CHROMA_FACTOR_422; break;  // 422
        default: chroma_factor = CHROMA_FACTOR_420; break; // 420
    }
    
    // Frame size in bytes
    int64_t frame_size = (width * height * bps * (int)chroma_factor) / 8;
    
    // DPB size (decoded picture buffer)
    int dpb_count = BASE_DPB_COUNT + queue_depth + 1;
    int64_t dpb_total = dpb_count * (frame_size + OVERHEAD_PER_FRAME);
    
    // Worker scratch memory
    int64_t scratch = num_workers * PER_WORKER_SCRATCH;
    
    // Decoder overhead
    int64_t overhead = DECODER_CONTEXT_SIZE + ENTROPY_CONTEXTS_SIZE + TABLES_SIZE;
    
    // Total with headroom
    int64_t total = (dpb_total + scratch + overhead);
    total = (int64_t)(total * HEADROOM_FACTOR);
    
    return (size_t)total;
}

bool av1_mem_is_initialized(void) {
    return g_mem_header != NULL && g_mem_header->initialized;
}

void *av1_mem_get_base(void) {
    if (!g_mem_header || !g_mem_header->initialized) {
        return NULL;
    }
    return g_mem_header;
}

size_t av1_mem_get_total_size(void) {
    if (!g_mem_header || !g_mem_header->initialized) {
        return 0;
    }
    return g_mem_header->stats.total_size;
}

// ============================================================================
// AOM Override Functions
// ============================================================================

void av1_mem_set_override_enabled(bool enabled) {
    if (g_mem_header && g_mem_header->initialized) {
        pthread_mutex_lock(&g_mem_header->mutex);
        g_mem_header->override_enabled = enabled;
        pthread_mutex_unlock(&g_mem_header->mutex);
    }
}

bool av1_mem_get_override_enabled(void) {
    if (!g_mem_header || !g_mem_header->initialized) {
        return false;
    }
    bool enabled;
    pthread_mutex_lock(&g_mem_header->mutex);
    enabled = g_mem_header->override_enabled;
    pthread_mutex_unlock(&g_mem_header->mutex);
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
    return memalign(alignment, size);
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
