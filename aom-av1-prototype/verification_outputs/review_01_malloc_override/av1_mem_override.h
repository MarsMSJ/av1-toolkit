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
