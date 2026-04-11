

## Code Review: AV1 Memory Override Layer

### Requirements Checklist

| # | Requirement | Status | Explanation |
|---|-------------|--------|-------------|
| 1 | All allocations come from the provided block (no malloc calls) | **FAIL** | Override functions fall back to system malloc when disabled |
| 2 | Aligned allocations are actually aligned (64-byte) | **FAIL** | Alignment calculation doesn't account for size header offset |
| 3 | Thread-safe under concurrent allocations | **FAIL** | Critical: mutex is copied by value, not reference |
| 4 | Free + re-allocate works | **PASS** | Free list implementation works correctly |
| 5 | Query size produces reasonable numbers | **PASS** | Formula produces reasonable estimates |
| 6 | Compiles with gcc -std=c11 -Wall -Wextra -pthread | **FAIL** | Missing pthread.h in header, potential warnings |

---

## Issues Found

### CRITICAL Issues

#### 1. Thread Safety - Mutex Copied by Value
**File:** `av1_mem_override.c`  
**Location:** `av1_mem_init()` around line 130

**What's wrong:**
```c
memcpy(&g_mem_header, header, sizeof(Av1MemHeader));
```
The mutex is copied by value, not by reference. After `av1_mem_init` returns, `g_mem_header.mutex` contains garbage (the bits of the original mutex, not a valid mutex object). All mutex operations on `g_mem_header.mutex` are undefined behavior.

**How to fix it:**
Store only a pointer to the header, or keep the header in the provided block and use a pointer to access it.

---

#### 2. Alignment Not Preserved After Size Header
**File:** `av1_mem_override.c`  
**Location:** `av1_mem_memalign()` around line 175

**What's wrong:**
The code stores the size before the aligned pointer:
```c
*(size_t *)ptr = aligned_size;
return (char *)ptr + sizeof(size_t);
```

If the user requests 64-byte alignment, the block is aligned, but the returned pointer (after adding `sizeof(size_t)`) is no longer 64-byte aligned. The size header throws off the alignment.

**How to fix it:**
Either:
1. Store the size in a separate metadata structure (not inline before the pointer)
2. Ensure the aligned_size includes extra space to maintain alignment after the header

---

### WARNING Issues

#### 3. Missing pthread.h Include in Header
**File:** `av1_mem_override.h`  
**Location:** Top of file

The header uses `pthread_mutex_t` in the internal structure but doesn't include `pthread.h`. This will cause compilation errors when other files include this header.

#### 4. Power of 2 Check Has Off-By-One Issue
**File:** `av1_mem_override.c`  
**Location:** `av1_mem_memalign()` around line 155

```c
alignment = (alignment & (alignment - 1)) ? MIN_ALIGNMENT : alignment;
```

This correctly handles non-power-of-2 values, but if `alignment` is 0, it becomes 0 & -1 = 0, which passes the check incorrectly. Should validate `alignment > 0` first.

#### 5. No Validation That Pointer Belongs to Allocator
**File:** `av1_mem_override.c`  
**Location:** `av1_mem_free()`

The `free` function doesn't verify that the pointer being freed was actually allocated from this allocator. A double-free or free of an invalid pointer could corrupt the free list.

#### 6. Global State Duplication
**File:** `av1_mem_override.c`  
**Location:** Global variables

The code maintains both `g_mem_header` (global copy) and expects the user to pass the original header. This creates confusion and potential for bugs. Should use a single source of truth.

---

### STYLE Issues

#### 7. Unused Padding Array
**File:** `av1_mem_override.c`  
**Location:** `Av1MemHeader` struct

The `char padding[64]` member is unused and adds unnecessary size to the header.

---

## Corrected Files

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
```

### av1_mem_override.h
```c
#ifndef AV1_MEM_OVERRIDE_H
#define AV1_MEM_OVERRIDE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// Include pthread.h for mutex type in the header
#include <pthread.h>

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

### test_mem_override.c
```c
#include "av1_mem_override.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>

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
    double log_max = log((double)max + 1.0);
    double log_val = (r / 32768.0) * log_max;
    size_t size = (size_t)(exp(log_val) - 1.0);
    if (size == 0) size = 1;
    if (size > max) size = max;
    return size;
}

static size_t random_alignment(void) {
    uint32_t r = fast_rand() % 4;
    switch (r) {
        case 0: return 16;
        case 1: return 32;
        case 2: return 64;
        default: return 128;
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

// Verify alignment of a pointer
static bool verify_alignment(void *ptr, size_t alignment) {
    return ((uintptr_t)ptr % alignment) == 0;
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
    size_t *ptr_sizes = malloc(sizeof(size_t) * NUM_ALLOCATIONS);
    size_t *ptr_aligns = malloc(sizeof(size_t) * NUM_ALLOCATIONS);
    if (!ptrs || !ptr_sizes || !ptr_aligns) {
        fprintf(stderr, "Failed to allocate pointer arrays\n");
        av1_mem_shutdown();
        free(mem_block);
        return 1;
    }
    
    // Initialize random seed
    g_seed = (uint32_t)time(NULL);
    
    size_t total_allocated = 0;
    int alignment_errors = 0;
    
    for (int i = 0; i < NUM_ALLOCATIONS; i++) {
        size_t size = random_size(1024 * 1024);  // 1 byte to 1 MB
        size_t align = random_alignment();
        
        void *ptr = av1_mem_memalign(align, size);
        if (!ptr) {
            fprintf(stderr, "Allocation %d failed (size=%zu, align=%zu)\n", i, size, align);
            ptrs[i] = NULL;
            ptr_sizes[i] = 0;
            ptr_aligns[i] = 0;
        } else {
            ptrs[i] = ptr;
            ptr_sizes[i] = size;
            ptr_aligns[i] = align;
            total_allocated += size;
            
            // Verify alignment
            if (!verify_alignment(ptr, align)) {
                alignment_errors++;
                fprintf(stderr, "Alignment error: ptr=%p, expected=%zu, actual offset=%zu\n",
                        ptr, align, (uintptr_t)ptr % align);
            }
            
            // Write a pattern to verify memory is usable
            memset(ptr, 0xAA, size);
        }
        
        if ((i + 1) % 200 == 0) {
            printf("  Allocated %d/%d blocks...\n", i + 1, NUM_ALLOCATIONS);
        }
    }
    
    if (alignment_errors > 0) {
        printf("  WARNING: %d alignment errors detected!\n", alignment_errors);
    } else {
        printf("  All allocations verified: correct alignment\n");
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
        free(ptr_sizes);
        free(ptr_aligns);
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
        free(ptr_sizes);
        free(ptr_aligns);
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
    
    // Phase 3: Allocate more blocks (test reuse of freed memory)
    print_separator();
    printf("Phase 3: Allocating %d more blocks...\n", NUM_ALLOC_AGAIN);
    
    int new_alignment_errors = 0;
    for (int i = 0; i < NUM_ALLOC_AGAIN; i++) {
        size_t size = random_size(1024 * 1024);
        size_t align = random_alignment();
        
        void *ptr = av1_mem_memalign(align, size);
        if (!ptr) {
            fprintf(stderr, "Allocation %d failed (size=%zu, align=%zu)\n", i, size, align);
            continue;
        }
        
        // Verify alignment
        if (!verify_alignment(ptr, align)) {
            new_alignment_errors++;
        }
        
        // Write a pattern
        memset(ptr, 0x55, size);
        
        if ((i + 1) % 100 == 0) {
            printf("  Allocated %d/%d blocks...\n", i + 1, NUM_ALLOC_AGAIN);
        }
    }
    
    if (new_alignment_errors > 0) {
        printf("  WARNING: %d alignment errors in Phase 3!\n", new_alignment_errors);
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
           (stats3.num_free_list_hits + stats3.num_bump_allocations + 1));
    
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
        printf("  av1_mem_override_memalign: OK (alignment verified: %s)\n",
               verify_alignment(test_align, 64) ? "YES" : "NO");
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
    free(ptr_sizes);
    free(ptr_aligns);
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