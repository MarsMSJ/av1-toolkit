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
