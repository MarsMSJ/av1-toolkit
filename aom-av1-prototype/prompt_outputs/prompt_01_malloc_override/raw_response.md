

### av1_mem_override.h
```c
#ifndef AV1_MEM_OVERRIDE_H
#define AV1_MEM_OVERRIDE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Stream information for memory estimation
typedef struct Av1StreamInfo {
    int width;
    int height;
    int max_bitrate;        // bits per pixel (typically 8-12 for 8-bit, 10-12 for 10-bit)
    int chroma_subsampling; // 0 = 420, 1 = 422, 2 = 444
    bool is_16bit;
} Av1StreamInfo;

// Memory statistics
typedef struct Av1MemStats {
    size_t total_size;
    size_t used_size;
    size_t peak_usage;
    size_t num_allocations;
    size_t num_frees;
    size_t num_free_list_hits;
    size_t num_bump_allocations;
    size_t largest_free_block;
} Av1MemStats;

// Initialize the memory allocator with a caller-provided block
// Returns true on success, false on failure
bool av1_mem_init(void *base, size_t size);

// Shutdown the allocator and release resources
void av1_mem_shutdown(void);

// Allocate memory (bump allocator with free-list fallback)
void *av1_mem_malloc(size_t size);

// Allocate aligned memory
void *av1_mem_memalign(size_t alignment, size_t size);

// Allocate zero-initialized memory
void *av1_mem_calloc(size_t num, size_t size);

// Free memory (add to free-list)
void av1_mem_free(void *ptr);

// Query current memory statistics
Av1MemStats av1_mem_get_stats(void);

// Reset statistics (but not the allocator state)
void av1_mem_reset_stats(void);

// Estimate memory requirements for a given stream
// queue_depth: number of frames to buffer for reordering
// num_workers: number of decoder worker threads
size_t av1_mem_query_size(const Av1StreamInfo *info, int queue_depth, int num_workers);

// Check if the allocator is initialized
bool av1_mem_is_initialized(void);

// Get the base pointer and total size
void *av1_mem_get_base(void);
size_t av1_mem_get_total_size(void);

// AOM memory function overrides (to be called from aom_mem.c)
// These are the functions that will replace the standard aom_* functions
void *av1_mem_override_malloc(size_t size);
void *av1_mem_override_memalign(size_t alignment, size_t size);
void *av1_mem_override_calloc(size_t num, size_t size);
void av1_mem_override_free(void *ptr);

// Enable/disable the memory override
void av1_mem_set_override_enabled(bool enabled);
bool av1_mem_get_override_enabled(void);

#ifdef __cplusplus
}
#endif

#endif // AV1_MEM_OVERRIDE_H
```

### av1_mem_override.c
```c
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
    
    // Padding to align the data region
    char padding[64];
} Av1MemHeader;

// ============================================================================
// Global State
// ============================================================================

static Av1MemHeader g_mem_header;
static bool g_header_initialized = false;

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
            // Could add to free list pool, but for simplicity we just leak
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

// ============================================================================
// Public API Implementation
// ============================================================================

bool av1_mem_init(void *base, size_t size) {
    if (!base || size < sizeof(Av1MemHeader) + 1024) {
        fprintf(stderr, "av1_mem_init: invalid base or size too small\n");
        return false;
    }
    
    // Initialize header
    Av1MemHeader *header = (Av1MemHeader *)base;
    
    memset(header, 0, sizeof(Av1MemHeader));
    
    // Set up bump allocator region (after header)
    header->bump_ptr = (char *)base + sizeof(Av1MemHeader);
    header->bump_end = (char *)base + size;
    
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
    
    // Copy to global state
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
    
    // Ensure alignment is at least MIN_ALIGNMENT and a power of 2
    if (alignment < MIN_ALIGNMENT) {
        alignment = MIN_ALIGNMENT;
    }
    alignment = (alignment & (alignment - 1)) ? MIN_ALIGNMENT : alignment;
    
    // Calculate aligned size including space for size header
    size_t aligned_size = align_size(size + sizeof(size_t), alignment);
    
    void *ptr = NULL;
    
    pthread_mutex_lock(&g_mem_header.mutex);
    
    // First, try to find a suitable block in the free list
    FreeBlock *block = find_best_fit(&g_mem_header.free_list, aligned_size);
    
    if (block) {
        // Use the free block
        ptr = (void *)block;
        g_mem_header.stats.num_free_list_hits++;
        
        // If block is larger than needed, split it
        if (block->size > aligned_size + sizeof(FreeBlock) + MIN_ALIGNMENT) {
            FreeBlock *remaining = (FreeBlock *)((char *)ptr + aligned_size);
            remaining->size = block->size - aligned_size;
            remaining->next = g_mem_header.free_list;
            g_mem_header.free_list = remaining;
        }
    } else {
        // Try bump allocator
        void *aligned_ptr = align_ptr(g_mem_header.bump_ptr, alignment);
        char *new_bump = (char *)aligned_ptr + aligned_size;
        
        if (new_bump <= (char *)g_mem_header.bump_end) {
            ptr = aligned_ptr;
            g_mem_header.bump_ptr = new_bump;
            g_mem_header.stats.num_bump_allocations++;
        } else {
            // Out of memory
            pthread_mutex_unlock(&g_mem_header.mutex);
            fprintf(stderr, "av1_mem_memalign: out of memory\n");
            return NULL;
        }
    }
    
    // Store size at the beginning of the allocation
    *(size_t *)ptr = aligned_size;
    
    // Update statistics
    g_mem_header.stats.used_size += aligned_size;
    g_mem_header.stats.num_allocations++;
    
    if (g_mem_header.stats.used_size > g_mem_header.stats.peak_usage) {
        g_mem_header.stats.peak_usage = g_mem_header.stats.used_size;
    }
    
    pthread_mutex_unlock(&g_mem_header.mutex);
    
    // Return pointer past the size header
    return (char *)ptr + sizeof(size_t);
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
    
    // Get the actual block pointer (before size header)
    char *block = (char *)ptr - sizeof(size_t);
    size_t block_size = *(size_t *)block;
    
    pthread_mutex_lock(&g_mem_header.mutex);
    
    // Add to free list
    FreeBlock *free_block = (FreeBlock *)block;
    free_block->size = block_size;
    free_block->next = g_mem_header.free_list;
    g_mem_header.free_list = free_block;
    
    // Update statistics
    g_mem_header.stats.used_size -= block_size;
    g_mem_header.stats.num_frees++;
    
    // Try to merge adjacent free blocks
    merge_free_blocks(&g_mem_header);
    
    // Update largest free block
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
    Av1MemStats stats = {0};
    
    if (g_header_initialized) {
        pthread_mutex_lock(&g_mem_header.mutex);
        stats = g_mem_header.stats;
        
        // Calculate fragmentation as ratio of largest free block to total free
        size_t total_free = g_mem_header.stats.total_size - g_mem_header.stats.used_size;
        if (total_free > 0 && stats.largest_free_block > 0) {
            // Fragmentation = 1 - (largest_free / total_free)
            // Lower is better
            double frag = 1.0 - ((double)stats.largest_free_block / (double)total_free);
            (void)frag; // Could be used for reporting
        }
        
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
    return g_header_initialized && g_mem_header.initialized;
}

void *av1_mem_get_base(void) {
    if (!g_header_initialized) {
        return NULL;
    }
    return &g_mem_header;
}

size_t av1_mem_get_total_size(void) {
    if (!g_header_initialized) {
        return 0;
    }
    return g_mem_header.stats.total_size;
}

// ============================================================================
// AOM Override Functions
// ============================================================================

void av1_mem_set_override_enabled(bool enabled) {
    if (g_header_initialized) {
        pthread_mutex_lock(&g_mem_header.mutex);
        g_mem_header.override_enabled = enabled;
        pthread_mutex_unlock(&g_mem_header.mutex);
    }
}

bool av1_mem_get_override_enabled(void) {
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
```

### test_mem_override.c
```c
#include "av1_mem_override.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <stdbool.h>

#define TEST_BLOCK_SIZE    (256 * 1024 * 1024)  // 256 MB
#define NUM_ALLOCATIONS    1000
#define NUM_FREE_HALF      500
#define NUM_ALLOC_AGAIN    500

// Random number generation with a simple LCG
static uint32_t g_seed = 12345;

static uint32_t fast_rand(void) {
    g_seed = g_seed * 1103515245 + 12345;
    return (g_seed >> 16) & 0x7FFF;
}

static size_t random_size(size_t max) {
    // Generate sizes from 1 byte to max, with logarithmic distribution
    // to get more interesting test patterns
    uint32_t r = fast_rand();
    double log_max = __builtin_log((double)max + 1.0);
    double log_val = (r / 32768.0) * log_max;
    size_t size = (size_t)(__builtin_exp(log_val) - 1.0);
    if (size == 0) size = 1;
    if (size > max) size = max;
    return size;
}

static size_t random_alignment(void) {
    uint32_t r = fast_rand() % 4;
    switch (r) {
        case 0: return 1;
        case 1: return 4;
        case 2: return 16;
        default: return 64;
    }
}

static void print_separator(void) {
    printf("============================================================\n");
}

static void print_stats(const char *phase, Av1MemStats *stats) {
    printf("\n--- %s ---\n", phase);
    printf("  Total size:      %zu bytes (%.2f MB)\n", 
           stats->total_size, stats->total_size / (1024.0 * 1024.0));
    printf("  Used size:       %zu bytes (%.2f MB)\n", 
           stats->used_size, stats->used_size / (1024.0 * 1024.0));
    printf("  Peak usage:      %zu bytes (%.2f MB)\n", 
           stats->peak_usage, stats->peak_usage / (1024.0 * 1024.0));
    printf("  Allocations:     %zu\n", stats->num_allocations);
    printf("  Frees:           %zu\n", stats->num_frees);
    printf("  Free-list hits:  %zu\n", stats->num_free_list_hits);
    printf("  Bump allocs:     %zu\n", stats->num_bump_allocations);
    printf("  Largest free:    %zu bytes (%.2f KB)\n", 
           stats->largest_free_block, stats->largest_free_block / 1024.0);
    
    // Calculate fragmentation
    size_t total_free = stats->total_size - stats->used_size;
    if (total_free > 0 && stats->largest_free_block > 0) {
        double fragmentation = 1.0 - ((double)stats->largest_free_block / (double)total_free);
        printf("  Fragmentation:   %.2f%%\n", fragmentation * 100.0);
    }
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    
    printf("AV1 Memory Override Test\n");
    print_separator();
    
    // Allocate the memory block
    printf("Allocating %d MB memory block...\n", TEST_BLOCK_SIZE / (1024 * 1024));
    void *mem_block = malloc(TEST_BLOCK_SIZE);
    if (!mem_block) {
        fprintf(stderr, "Failed to allocate memory block\n");
        return 1;
    }
    
    printf("Memory block at: %p\n", mem_block);
    
    // Initialize the allocator
    printf("Initializing allocator...\n");
    if (!av1_mem_init(mem_block, TEST_BLOCK_SIZE)) {
        fprintf(stderr, "Failed to initialize memory allocator\n");
        free(mem_block);
        return 1;
    }
    
    // Test memory query function
    printf("\n--- Memory Query Test ---\n");
    Av1StreamInfo info = {
        .width = 1920,
        .height = 1080,
        .max_bitrate = 10,
        .chroma_subsampling = 0,  // 420
        .is_16bit = false
    };
    
    size_t estimated = av1_mem_query_size(&info, 4, 4);
    printf("Estimated memory for 1080p, queue_depth=4, workers=4: %zu bytes (%.2f MB)\n",
           estimated, estimated / (1024.0 * 1024.0));
    
    // Test with different resolutions
    Av1StreamInfo info_720p = { .width = 1280, .height = 720, .max_bitrate = 8, .chroma_subsampling = 0 };
    Av1StreamInfo info_4k = { .width = 3840, .height = 2160, .max_bitrate = 12, .chroma_subsampling = 0 };
    
    size_t est_720p = av1_mem_query_size(&info_720p, 4, 4);
    size_t est_4k = av1_mem_query_size(&info_4k, 4, 4);
    
    printf("Estimated memory for 720p:  %zu bytes (%.2f MB)\n", est_720p, est_720p / (1024.0 * 1024.0));
    printf("Estimated memory for 4K:    %zu bytes (%.2f MB)\n", est_4k, est_4k / (1024.0 * 1024.0));
    
    // Phase 1: Initial allocations
    print_separator();
    printf("Phase 1: Allocating %d random blocks...\n", NUM_ALLOCATIONS);
    
    void **ptrs = malloc(sizeof(void *) * NUM_ALLOCATIONS);
    if (!ptrs) {
        fprintf(stderr, "Failed to allocate pointer array\n");
        av1_mem_shutdown();
        free(mem_block);
        return 1;
    }
    
    // Initialize random seed
    g_seed = (uint32_t)time(NULL);
    
    size_t total_allocated = 0;
    for (int i = 0; i < NUM_ALLOCATIONS; i++) {
        size_t size = random_size(1024 * 1024);  // 1 byte to 1 MB
        size_t align = random_alignment();
        
        void *ptr = av1_mem_memalign(align, size);
        if (!ptr) {
            fprintf(stderr, "Allocation %d failed (size=%zu, align=%zu)\n", i, size, align);
            // Continue with remaining allocations
            ptrs[i] = NULL;
        } else {
            ptrs[i] = ptr;
            total_allocated += size;
            
            // Write a pattern to verify memory is usable
            memset(ptr, 0xAA, size);
        }
        
        if ((i + 1) % 200 == 0) {
            printf("  Allocated %d/%d blocks...\n", i + 1, NUM_ALLOCATIONS);
        }
    }
    
    Av1MemStats stats1 = av1_mem_get_stats();
    print_stats("After Phase 1 (initial allocations)", &stats1);
    printf("  Requested total: %zu bytes (%.2f MB)\n", 
           total_allocated, total_allocated / (1024.0 * 1024.0));
    
    // Phase 2: Free half of the allocations
    print_separator();
    printf("Phase 2: Freeing %d random blocks...\n", NUM_FREE_HALF);
    
    int freed_count = 0;
    int *freed_indices = malloc(sizeof(int) * NUM_FREE_HALF);
    if (!freed_indices) {
        fprintf(stderr, "Failed to allocate freed indices array\n");
        free(ptrs);
        av1_mem_shutdown();
        free(mem_block);
        return 1;
    }
    
    // Mark all as not freed
    bool *was_freed = calloc(NUM_ALLOCATIONS, sizeof(bool));
    if (!was_freed) {
        fprintf(stderr, "Failed to allocate was_freed array\n");
        free(freed_indices);
        free(ptrs);
        av1_mem_shutdown();
        free(mem_block);
        return 1;
    }
    
    // Randomly select blocks to free
    for (int i = 0; i < NUM_FREE_HALF; i++) {
        int idx;
        do {
            idx = fast_rand() % NUM_ALLOCATIONS;
        } while (was_freed[idx] || ptrs[idx] == NULL);
        
        av1_mem_free(ptrs[idx]);
        was_freed[idx] = true;
        freed_indices[freed_count++] = idx;
        ptrs[idx] = NULL;
        
        if ((i + 1) % 100 == 0) {
            printf("  Freed %d/%d blocks...\n", i + 1, NUM_FREE_HALF);
        }
    }
    
    Av1MemStats stats2 = av1_mem_get_stats();
    print_stats("After Phase 2 (freed half)", &stats2);
    
    // Phase 3: Allocate more blocks
    print_separator();
    printf("Phase 3: Allocating %d more blocks...\n", NUM_ALLOC_AGAIN);
    
    for (int i = 0; i < NUM_ALLOC_AGAIN; i++) {
        size_t size = random_size(1024 * 1024);
        size_t align = random_alignment();
        
        void *ptr = av1_mem_memalign(align, size);
        if (!ptr) {
            fprintf(stderr, "Allocation %d failed (size=%zu, align=%zu)\n", i, size, align);
            continue;
        }
        
        // Write a pattern
        memset(ptr, 0x55, size);
        
        if ((i + 1) % 100 == 0) {
            printf("  Allocated %d/%d blocks...\n", i + 1, NUM_ALLOC_AGAIN);
        }
    }
    
    Av1MemStats stats3 = av1_mem_get_stats();
    print_stats("After Phase 3 (additional allocations)", &stats3);
    
    // Phase 4: Summary
    print_separator();
    printf("FINAL SUMMARY\n");
    print_separator();
    
    printf("Total allocations attempted:  %d + %d = %d\n", 
           NUM_ALLOCATIONS, NUM_ALLOC_AGAIN, NUM_ALLOCATIONS + NUM_ALLOC_AGAIN);
    printf("Total frees:                  %d\n", NUM_FREE_HALF);
    printf("Peak memory usage:            %.2f MB (%.2f%% of block)\n",
           stats3.peak_usage / (1024.0 * 1024.0),
           (stats3.peak_usage * 100.0) / stats3.total_size);
    printf("Final used memory:            %.2f MB\n",
           stats3.used_size / (1024.0 * 1024.0));
    
    // Calculate efficiency
    double efficiency = (stats3.used_size * 100.0) / stats3.total_size;
    double fragmentation = 0;
    size_t total_free = stats3.total_size - stats3.used_size;
    if (total_free > 0 && stats3.largest_free_block > 0) {
        fragmentation = 1.0 - ((double)stats3.largest_free_block / (double)total_free);
    }
    
    printf("Memory efficiency:            %.2f%%\n", efficiency);
    printf("Fragmentation:                %.2f%%\n", fragmentation * 100.0);
    printf("Free-list reuse ratio:        %.2f%%\n",
           stats3.num_free_list_hits * 100.0 / 
           (stats3.num_free_list_hits + stats3.num_bump_allocations));
    
    // Test override functions
    print_separator();
    printf("Testing AOM override functions...\n");
    
    av1_mem_set_override_enabled(true);
    void *test_override = av1_mem_override_malloc(1024);
    if (test_override) {
        printf("  av1_mem_override_malloc: OK\n");
        av1_mem_override_free(test_override);
    } else {
        printf("  av1_mem_override_malloc: FAILED\n");
    }
    
    void *test_calloc = av1_mem_override_calloc(10, 100);
    if (test_calloc) {
        printf("  av1_mem_override_calloc: OK\n");
        av1_mem_override_free(test_calloc);
    } else {
        printf("  av1_mem_override_calloc: FAILED\n");
    }
    
    void *test_align = av1_mem_override_memalign(64, 2048);
    if (test_align) {
        printf("  av1_mem_override_memalign: OK\n");
        av1_mem_override_free(test_align);
    } else {
        printf("  av1_mem_override_memalign: FAILED\n");
    }
    
    // Test disabling override
    av1_mem_set_override_enabled(false);
    void *test_disabled = av1_mem_override_malloc(1024);
    if (test_disabled) {
        printf("  Override disabled (using system malloc): OK\n");
        free(test_disabled);  // Must use system free when disabled
    }
    
    // Cleanup
    print_separator();
    printf("Cleaning up...\n");
    
    // Free remaining allocations
    for (int i = 0; i < NUM_ALLOCATIONS; i++) {
        if (ptrs[i] != NULL) {
            av1_mem_free(ptrs[i]);
        }
    }
    
    free(ptrs);
    free(freed_indices);
    free(was_freed);
    
    Av1MemStats final_stats = av1_mem_get_stats();
    print_stats("After cleanup", &final_stats);
    
    av1_mem_shutdown();
    free(mem_block);
    
    printf("\nTest completed successfully!\n");
    
    return 0;
}
```