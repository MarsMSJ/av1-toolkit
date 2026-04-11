#include "av1_decoder_api.h"
#include "av1_mem_override.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

// Test configuration
#define TEST_WIDTH      1920
#define TEST_HEIGHT     1080
#define TEST_QUEUE_DEPTH 4
#define TEST_WORKERS    4

static void print_separator(void) {
    printf("============================================================\n");
}

static void print_memory_breakdown(const char *title, const Av1MemoryRequirements *req) {
    printf("\n--- %s ---\n", title);
    printf("Total required:     %12zu bytes (%10.2f MB)\n", 
           req->total_size, req->total_size / (1024.0 * 1024.0));
    printf("Alignment:          %zu bytes\n", req->alignment);
    printf("\nBreakdown:\n");
    printf("  Frame buffers:    %12zu bytes (%zu buffers)\n", 
           req->breakdown.frame_buffers, req->breakdown.dpb_count);
    printf("  Worker scratch:   %12zu bytes (%zu workers)\n", 
           req->breakdown.worker_scratch, req->breakdown.worker_count);
    printf("  Entropy contexts: %12zu bytes\n", req->breakdown.entropy_contexts);
    printf("  Decoder context:  %12zu bytes\n", req->breakdown.decoder_context);
    printf("  Tile data:        %12zu bytes\n", req->breakdown.tile_data);
    printf("  Mode info grid:   %12zu bytes\n", req->breakdown.mode_info_grid);
    printf("  Other:            %12zu bytes\n", req->breakdown.other);
    
    // Calculate percentages
    double total = (double)req->total_size;
    printf("\nPercentages:\n");
    printf("  Frame buffers:    %6.2f%%\n", (req->breakdown.frame_buffers / total) * 100.0);
    printf("  Worker scratch:   %6.2f%%\n", (req->breakdown.worker_scratch / total) * 100.0);
    printf("  Entropy contexts: %6.2f%%\n", (req->breakdown.entropy_contexts / total) * 100.0);
    printf("  Decoder context:  %6.2f%%\n", (req->breakdown.decoder_context / total) * 100.0);
    printf("  Tile data:        %6.2f%%\n", (req->breakdown.tile_data / total) * 100.0);
    printf("  Mode info grid:   %6.2f%%\n", (req->breakdown.mode_info_grid / total) * 100.0);
    printf("  Other:            %6.2f%%\n", (req->breakdown.other / total) * 100.0);
}

static void test_query_memory(void) {
    print_separator();
    printf("TEST: av1_query_memory\n");
    print_separator();
    
    // Test 1: 1080p 8-bit 4:2:0
    printf("\nTest 1: 1080p 8-bit 4:2:0\n");
    Av1StreamInfo info_1080p = {
        .width = TEST_WIDTH,
        .height = TEST_HEIGHT,
        .max_bitrate = 8,
        .chroma_subsampling = 0,  // 420
        .is_16bit = false
    };
    
    Av1MemoryRequirements req_1080p = av1_query_memory(&info_1080p, TEST_QUEUE_DEPTH, TEST_WORKERS);
    print_memory_breakdown("1080p 8-bit 4:2:0", &req_1080p);
    
    // Test 2: 720p 8-bit 4:2:0
    printf("\n\nTest 2: 720p 8-bit 4:2:0\n");
    Av1StreamInfo info_720p = {
        .width = 1280,
        .height = 720,
        .max_bitrate = 8,
        .chroma_subsampling = 0,
        .is_16bit = false
    };
    
    Av1MemoryRequirements req_720p = av1_query_memory(&info_720p, TEST_QUEUE_DEPTH, TEST_WORKERS);
    printf("Total required: %zu bytes (%.2f MB)\n", 
           req_720p.total_size, req_720p.total_size / (1024.0 * 1024.0));
    
    // Test 3: 4K 10-bit 4:2:0
    printf("\n\nTest 3: 4K 10-bit 4:2:0\n");
    Av1StreamInfo info_4k = {
        .width = 3840,
        .height = 2160,
        .max_bitrate = 10,
        .chroma_subsampling = 0,
        .is_16bit = false  // 10-bit uses 2 bytes but is_16bit specifically means 12-bit
    };
    
    Av1MemoryRequirements req_4k = av1_query_memory(&info_4k, TEST_QUEUE_DEPTH, TEST_WORKERS);
    printf("Total required: %zu bytes (%.2f MB)\n", 
           req_4k.total_size, req_4k.total_size / (1024.0 * 1024.0));
    
    // Test 4: 1080p 10-bit 4:2:2
    printf("\n\nTest 4: 1080p 10-bit 4:2:2\n");
    Av1StreamInfo info_422 = {
        .width = TEST_WIDTH,
        .height = TEST_HEIGHT,
        .max_bitrate = 10,
        .chroma_subsampling = 1,  // 422
        .is_16bit = false
    };
    
    Av1MemoryRequirements req_422 = av1_query_memory(&info_422, TEST_QUEUE_DEPTH, TEST_WORKERS);
    printf("Total required: %zu bytes (%.2f MB)\n", 
           req_422.total_size, req_422.total_size / (1024.0 * 1024.0));
    
    // Test 5: Different queue depths
    printf("\n\nTest 5: Memory vs Queue Depth (1080p)\n");
    for (int qd = 0; qd <= 8; qd += 2) {
        Av1MemoryRequirements req = av1_query_memory(&info_1080p, qd, TEST_WORKERS);
        printf("  queue_depth=%d: %zu bytes (%.2f MB)\n", 
               qd, req.total_size, req.total_size / (1024.0 * 1024.0));
    }
    
    // Test 6: Different worker counts
    printf("\n\nTest 6: Memory vs Worker Count (1080p, queue_depth=4)\n");
    for (int w = 1; w <= 8; w++) {
        Av1MemoryRequirements req = av1_query_memory(&info_1080p, TEST_QUEUE_DEPTH, w);
        printf("  workers=%d: %zu bytes (%.2f MB)\n", 
               w, req.total_size, req.total_size / (1024.0 * 1024.0));
    }
}

// Helper function to allocate memory using the override system
static void *test_allocate_memory(size_t size, size_t alignment) {
    // Initialize the memory system first
    if (!av1_mem_init(NULL, size + alignment)) {
        return NULL;
    }
    av1_mem_set_override_enabled(true);
    
    void *ptr = av1_mem_memalign(alignment, size);
    av1_mem_set_override_enabled(false);
    av1_mem_shutdown();
    
    return ptr;
}

// Helper function to free memory using the override system
static void test_free_memory(void *ptr, size_t size) {
    if (!ptr) return;
    
    // We need to reinitialize to properly free
    // In a real scenario, you'd track the allocator state
    // For testing, we'll just use the override if available
    av1_mem_set_override_enabled(true);
    av1_mem_free(ptr);
    av1_mem_set_override_enabled(false);
    av1_mem_shutdown();
}

static void test_create_decoder(void) {
    print_separator();
    printf("TEST: av1_create_decoder\n");
    print_separator();
    
    // First query memory requirements
    Av1StreamInfo info = {
        .width = TEST_WIDTH,
        .height = TEST_HEIGHT,
        .max_bitrate = 8,
        .chroma_subsampling = 0,
        .is_16bit = false
    };
    
    Av1MemoryRequirements req = av1_query_memory(&info, TEST_QUEUE_DEPTH, TEST_WORKERS);
    
    printf("\nQuery result: %zu bytes (%.2f MB)\n", 
           req.total_size, req.total_size / (1024.0 * 1024.0));
    
    // Allocate memory block using aligned_alloc (this is the test's responsibility,
    // the decoder will use the override for internal allocations)
    printf("\nAllocating memory block...\n");
    void *mem_block = aligned_alloc(req.alignment, req.total_size);
    if (!mem_block) {
        fprintf(stderr, "Failed to allocate memory block\n");
        return;
    }
    
    printf("Memory block allocated at: %p\n", mem_block);
    
    // Initialize memory to zero (good practice)
    memset(mem_block, 0, req.total_size);
    
    // Create decoder configuration
    Av1DecoderConfig config = {
        .memory_base = mem_block,
        .memory_size = req.total_size,
        .queue_depth = TEST_QUEUE_DEPTH,
        .num_worker_threads = TEST_WORKERS,
        .worker_config = {
            .priority = 0,
            .cpu_affinity = -1,
            .core_id = -1
        },
        .copy_thread_config = {
            .priority = 0,
            .cpu_affinity = -1,
            .core_id = -1
        },
        .use_gpu = false,  // Disable GPU for this test
        .gpu_device = 0,
        .gpu_thread_config = {
            .priority = 0,
            .cpu_affinity = -1,
            .core_id = -1
        },
        .enable_threading = true,
        .enable_frame_parallel = false,
        .max_tile_cols = 0,  // Auto
        .max_tile_rows = 0   // Auto
    };
    
    // Create decoder
    printf("\nCreating decoder...\n");
    Av1Decoder *decoder = av1_create_decoder(&config);
    
    if (!decoder) {
        fprintf(stderr, "Failed to create decoder\n");
        free(mem_block);  // Free the original allocation
        return;
    }
    
    printf("Decoder created successfully!\n");
    printf("  Handle: %p\n", (void*)decoder);
    
    // Verify decoder state
    Av1DecoderState state = av1_get_decoder_state(decoder);
    printf("  State: %d (READY=%d)\n", state, AV1_DECODER_STATE_READY);
    
    if (state != AV1_DECODER_STATE_READY) {
        fprintf(stderr, "Decoder state is not READY!\n");
    }
    
    // Get and print config
    const Av1DecoderConfig *retrieved_config = av1_get_decoder_config(decoder);
    if (retrieved_config) {
        printf("\nDecoder configuration:\n");
        printf("  Queue depth: %d\n", retrieved_config->queue_depth);
        printf("  Workers: %d\n", retrieved_config->num_worker_threads);
        printf("  GPU enabled: %s\n", retrieved_config->use_gpu ? "yes" : "no");
        printf("  Threading: %s\n", retrieved_config->enable_threading ? "yes" : "no");
    }
    
    // Get memory stats
    printf("\nMemory statistics after decoder creation:\n");
    Av1MemStats stats = av1_get_mem_stats(decoder);
    printf("  Total size:      %zu bytes (%.2f MB)\n", 
           stats.total_size, stats.total_size / (1024.0 * 1024.0));
    printf("  Used size:       %zu bytes (%.2f MB)\n", 
           stats.used_size, stats.used_size / (1024.0 * 1024.0));
    printf("  Peak usage:      %zu bytes (%.2f MB)\n", 
           stats.peak_usage, stats.peak_usage / (1024.0 * 1024.0));
    printf("  Allocations:     %zu\n", stats.num_allocations);
    printf("  Frees:           %zu\n", stats.num_frees);
    printf("  Free-list hits:  %zu\n", stats.num_free_list_hits);
    printf("  Bump allocs:     %zu\n", stats.num_bump_allocations);
    
    // Test with GPU enabled
    printf("\n--- Testing with GPU enabled ---\n");
    
    // Need more memory for GPU test
    Av1MemoryRequirements req_gpu = av1_query_memory(&info, TEST_QUEUE_DEPTH, TEST_WORKERS);
    // Add extra for GPU thread
    size_t gpu_extra = 16 * 1024 * 1024;  // 16MB extra
    size_t total_gpu = req_gpu.total_size + gpu_extra;
    total_gpu = (total_gpu + req_gpu.alignment - 1) & ~(req_gpu.alignment - 1);
    
    void *mem_block_gpu = aligned_alloc(req_gpu.alignment, total_gpu);
    if (mem_block_gpu) {
        memset(mem_block_gpu, 0, total_gpu);
        
        Av1DecoderConfig config_gpu = config;
        config_gpu.memory_base = mem_block_gpu;
        config_gpu.memory_size = total_gpu;
        config_gpu.use_gpu = true;
        
        Av1Decoder *decoder_gpu = av1_create_decoder(&config_gpu);
        if (decoder_gpu) {
            printf("GPU decoder created successfully!\n");
            
            const Av1DecoderConfig *cfg = av1_get_decoder_config(decoder_gpu);
            if (cfg) {
                printf("  GPU enabled: %s\n", cfg->use_gpu ? "yes" : "no");
            }
            
            // Destroy GPU decoder
            av1_destroy_decoder(decoder_gpu);
            printf("GPU decoder destroyed.\n");
        } else {
            printf("Failed to create GPU decoder (expected if GPU not available).\n");
        }
        
        free(mem_block_gpu);
    }
    
    // Destroy decoder
    printf("\nDestroying decoder...\n");
    int result = av1_destroy_decoder(decoder);
    printf("Destroy result: %d\n", result);
    
    // Free memory block - use free() since it was allocated with aligned_alloc
    // before the override was enabled
    free(mem_block);
    
    printf("\nDecoder test completed successfully!\n");
}

static void test_error_handling(void) {
    print_separator();
    printf("TEST: Error Handling\n");
    print_separator();
    
    // Test 1: NULL config
    printf("\nTest 1: NULL config\n");
    Av1Decoder *decoder = av1_create_decoder(NULL);
    if (decoder == NULL) {
        printf("  PASS: Correctly returned NULL for NULL config\n");
    } else {
        printf("  FAIL: Should have returned NULL\n");
        av1_destroy_decoder(decoder);
    }
    
    // Test 2: Invalid memory
    printf("\nTest 2: Invalid memory base\n");
    Av1DecoderConfig bad_config = {
        .memory_base = NULL,
        .memory_size = 1000000,
        .queue_depth = 4,
        .num_worker_threads = 4
    };
    decoder = av1_create_decoder(&bad_config);
    if (decoder == NULL) {
        printf("  PASS: Correctly returned NULL for invalid memory\n");
    } else {
        printf("  FAIL: Should have returned NULL\n");
        av1_destroy_decoder(decoder);
    }
    
    // Test 3: Memory too small
    printf("\nTest 3: Memory too small\n");
    void *small_mem = aligned_alloc(64, 1024);  // Way too small
    if (small_mem) {
        Av1DecoderConfig small_config = {
            .memory_base = small_mem,
            .memory_size = 1024,
            .queue_depth = 4,
            .num_worker_threads = 4
        };
        decoder = av1_create_decoder(&small_config);
        if (decoder == NULL) {
            printf("  PASS: Correctly returned NULL for small memory\n");
        } else {
            printf("  FAIL: Should have returned NULL\n");
            av1_destroy_decoder(decoder);
        }
        free(small_mem);
    }
    
    // Test 4: Query with invalid params
    printf("\nTest 4: Query with invalid params\n");
    Av1MemoryRequirements req = av1_query_memory(NULL, 4, 4);
    if (req.total_size == 0) {
        printf("  PASS: Correctly returned 0 for NULL info\n");
    } else {
        printf("  FAIL: Should have returned 0\n");
    }
    
    req = av1_query_memory(&(Av1StreamInfo){0}, -1, 4);
    if (req.total_size == 0) {
        printf("  PASS: Correctly returned 0 for negative queue_depth\n");
    } else {
        printf("  FAIL: Should have returned 0\n");
    }
    
    // Test 5: Get state of NULL decoder
    printf("\nTest 5: Get state of NULL decoder\n");
    Av1DecoderState state = av1_get_decoder_state(NULL);
    if (state == AV1_DECODER_STATE_UNINITIALIZED) {
        printf("  PASS: Correctly returned UNINITIALIZED for NULL decoder\n");
    } else {
        printf("  FAIL: Should have returned UNINITIALIZED\n");
    }
    
    // Test 6: Destroy NULL decoder
    printf("\nTest 6: Destroy NULL decoder\n");
    int result = av1_destroy_decoder(NULL);
    if (result == -1) {
        printf("  PASS: Correctly returned -1 for NULL decoder\n");
    } else {
        printf("  FAIL: Should have returned -1\n");
    }
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    
    printf("=== AV1 Decoder API Tests ===\n");
    printf("Testing: av1_query_memory and av1_create_decoder\n\n");
    
    // Run tests
    test_query_memory();
    printf("\n");
    
    test_create_decoder();
    printf("\n");
    
    test_error_handling();
    printf("\n");
    
    print_separator();
    printf("All tests completed!\n");
    print_separator();
    
    return 0;
}
