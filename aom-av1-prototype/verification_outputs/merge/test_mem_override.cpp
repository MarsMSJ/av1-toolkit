#include "av1_mem_override.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <stdbool.h>

#define TEST_BLOCK_SIZE (256 * 1024 * 1024)
#define NUM_ALLOCATIONS 1000
#define NUM_FREE_HALF 500
#define NUM_ALLOC_AGAIN 500

static uint32_t g_seed = 12345;

static uint32_t fast_rand(void) {
    g_seed = g_seed * 1103515245 + 12345;
    return (g_seed >> 16) & 0x7FFF;
}

static size_t random_size(size_t max) {
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

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    
    printf("AV1 Memory Override Test\n");
    printf("========================\n\n");
    
    void *mem_block = malloc(TEST_BLOCK_SIZE);
    if (!mem_block) {
        fprintf(stderr, "Failed to allocate memory block\n");
        return 1;
    }
    
    printf("Memory block at: %p\n", mem_block);
    
    if (!av1_mem_init(mem_block, TEST_BLOCK_SIZE)) {
        fprintf(stderr, "Failed to initialize memory allocator\n");
        free(mem_block);
        return 1;
    }
    
    Av1StreamInfo info = { .width = 1920, .height = 1080, .max_bitrate = 10, .chroma_subsampling = 0, .is_16bit = false };
    size_t estimated = av1_mem_query_size(&info, 4, 4);
    printf("Estimated memory for 1080p: %zu bytes (%.2f MB)\n", estimated, estimated / (1024.0 * 1024.0));
    
    g_seed = (uint32_t)time(NULL);
    
    void **ptrs = static_cast<void**>(malloc(sizeof(void *) * NUM_ALLOCATIONS));
    if (!ptrs) {
        av1_mem_shutdown();
        free(mem_block);
        return 1;
    }
    
    printf("\nPhase 1: Allocating %d random blocks...\n", NUM_ALLOCATIONS);
    
    for (int i = 0; i < NUM_ALLOCATIONS; i++) {
        size_t size = random_size(1024 * 1024);
        size_t align = random_alignment();
        
        ptrs[i] = av1_mem_memalign(align, size);
        if (ptrs[i]) {
            memset(ptrs[i], 0xAA, size);
        }
    }
    
    Av1MemStats stats1 = av1_mem_get_stats();
    printf("Used: %zu bytes, Peak: %zu bytes\n", stats1.used_size, stats1.peak_usage);
    
    printf("\nPhase 2: Freeing %d random blocks...\n", NUM_FREE_HALF);
    
    bool *was_freed = static_cast<bool*>(calloc(NUM_ALLOCATIONS, sizeof(bool)));
    for (int i = 0; i < NUM_FREE_HALF; i++) {
        int idx;
        do {
            idx = fast_rand() % NUM_ALLOCATIONS;
        } while (was_freed[idx] || ptrs[idx] == NULL);
        
        av1_mem_free(ptrs[idx]);
        was_freed[idx] = true;
        ptrs[idx] = NULL;
    }
    
    Av1MemStats stats2 = av1_mem_get_stats();
    printf("Used: %zu bytes, Largest free: %zu bytes\n", stats2.used_size, stats2.largest_free_block);
    
    printf("\nPhase 3: Allocating %d more blocks...\n", NUM_ALLOC_AGAIN);
    
    for (int i = 0; i < NUM_ALLOC_AGAIN; i++) {
        size_t size = random_size(1024 * 1024);
        size_t align = random_alignment();
        
        void *ptr = av1_mem_memalign(align, size);
        if (ptr) {
            memset(ptr, 0x55, size);
        }
    }
    
    Av1MemStats stats3 = av1_mem_get_stats();
    printf("Used: %zu bytes, Peak: %zu bytes\n", stats3.used_size, stats3.peak_usage);
    
    printf("\nTesting AOM override functions...\n");
    
    av1_mem_set_override_enabled(true);
    void *test_override = av1_mem_override_malloc(1024);
    if (test_override) {
        printf("  av1_mem_override_malloc: OK\n");
        av1_mem_override_free(test_override);
    }
    
    void *test_calloc = av1_mem_override_calloc(10, 100);
    if (test_calloc) {
        printf("  av1_mem_override_calloc: OK\n");
        av1_mem_override_free(test_calloc);
    }
    
    void *test_align = av1_mem_override_memalign(64, 2048);
    if (test_align) {
        printf("  av1_mem_override_memalign: OK\n");
        av1_mem_override_free(test_align);
    }
    
    for (int i = 0; i < NUM_ALLOCATIONS; i++) {
        if (ptrs[i] != NULL) {
            av1_mem_free(ptrs[i]);
        }
    }
    
    free(ptrs);
    free(was_freed);
    
    Av1MemStats final_stats = av1_mem_get_stats();
    printf("\nFinal: Used=%zu bytes, Peak=%zu bytes\n", final_stats.used_size, final_stats.peak_usage);
    
    av1_mem_shutdown();
    free(mem_block);
    
    printf("\nTest completed successfully!\n");
    
    return 0;
}
