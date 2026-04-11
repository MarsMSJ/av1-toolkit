#include "av1_decoder_api.h"
#include "av1_mem_override.h"
#include "ivf_parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

// Test configuration
#define TEST_WIDTH      1920
#define TEST_HEIGHT     1080
#define TEST_QUEUE_DEPTH 4
#define TEST_WORKERS    1  // Use single thread for decode test

static void print_separator(void) {
    printf("============================================================\n");
}

static void test_ivf_parser(const char *filename) {
    print_separator();
    printf("TEST: IVF Parser - %s\n", filename);
    print_separator();
    
    // Check if file is valid IVF
    if (!ivf_parser_is_valid(filename)) {
        printf("File is not a valid IVF file: %s\n", filename);
        return;
    }
    
    // Open parser
    IvfParser *parser = ivf_parser_open(filename);
    if (!parser) {
        printf("Failed to open IVF file\n");
        return;
    }
    
    // Get header
    const IvfHeader *header = ivf_parser_get_header(parser);
    if (header) {
        printf("\nIVF Header:\n");
        printf("  Resolution: %dx%d\n", header->width, header->height);
        printf("  FourCC: %.4s\n", header->fourcc);
        printf("  Timebase: %u/%u\n", header->timebase_num, header->timebase_den);
        printf("  Frames: %u\n", header->num_frames);
    }
    
    // Read first few frames
    printf("\nReading frames:\n");
    int frame_count = 0;
    const int max_frames_to_read = 10;
    
    while (!ivf_parser_eof(parser) && frame_count < max_frames_to_read) {
        uint8_t *data = NULL;
        size_t size = 0;
        uint64_t timestamp = 0;
        
        int result = ivf_parser_read_frame(parser, &data, &size, &timestamp);
        if (result == 0 && data) {
            printf("  Frame %d: size=%zu, timestamp=%lu\n", 
                   frame_count, size, (unsigned long)timestamp);
            free(data);
            frame_count++;
        } else {
            break;
        }
    }
    
    printf("  (Read %d frames, file has %d total)\n", 
           frame_count, ivf_parser_get_num_frames(parser));
    
    ivf_parser_close(parser);
}

static void test_decode_file(const char *filename) {
    print_separator();
    printf("TEST: av1_decode - %s\n", filename);
    print_separator();
    
    // First, query memory requirements
    Av1StreamInfo info = {
        .width = TEST_WIDTH,
        .height = TEST_HEIGHT,
        .max_bitrate = 10,
        .chroma_subsampling = 0,
        .is_16bit = false
    };
    
    Av1MemoryRequirements req = av1_query_memory(&info, TEST_QUEUE_DEPTH, TEST_WORKERS);
    printf("\nMemory required: %zu bytes (%.2f MB)\n", 
           req.total_size, req.total_size / (1024.0 * 1024.0));
    
    // Allocate memory block
    void *mem_block = aligned_alloc(req.alignment, req.total_size);
    if (!mem_block) {
        fprintf(stderr, "Failed to allocate memory block\n");
        return;
    }
    memset(mem_block, 0, req.total_size);
    
    // Create decoder configuration
    Av1DecoderConfig config = {
        .memory_base = mem_block,
        .memory_size = req.total_size,
        .queue_depth = TEST_QUEUE_DEPTH,
        .num_worker_threads = TEST_WORKERS,
        .worker_config = { .priority = 0, .cpu_affinity = -1, .core_id = -1 },
        .copy_thread_config = { .priority = 0, .cpu_affinity = -1, .core_id = -1 },
        .use_gpu = false,
        .gpu_device = 0,
        .gpu_thread_config = { .priority = 0, .cpu_affinity = -1, .core_id = -1 },
        .enable_threading = false,  // Single-threaded for this test
        .enable_frame_parallel = false,
        .max_tile_cols = 0,
        .max_tile_rows = 0
    };
    
    // Create decoder
    printf("\nCreating decoder...\n");
    Av1Decoder *decoder = av1_create_decoder(&config);
    if (!decoder) {
        fprintf(stderr, "Failed to create decoder\n");
        free(mem_block);
        return;
    }
    
    printf("Decoder created successfully!\n");
    
    // Open IVF file
    IvfParser *parser = ivf_parser_open(filename);
    if (!parser) {
        fprintf(stderr, "Failed to open IVF file: %s\n", filename);
        av1_destroy_decoder(decoder);
        free(mem_block);
        return;
    }
    
    const IvfHeader *header = ivf_parser_get_header(parser);
    printf("Decoding %u frames from %s (%dx%d)...\n",
           header->num_frames, filename, header->width, header->height);
    
    // Decode all frames
    printf("\n--- Decoding Frames ---\n");
    
    int frames_decoded = 0;
    int frames_with_output = 0;
    int queue_full_count = 0;
    
    while (!ivf_parser_eof(parser)) {
        uint8_t *data = NULL;
        size_t size = 0;
        uint64_t timestamp = 0;
        
        int result = ivf_parser_read_frame(parser, &data, &size, &timestamp);
        if (result != 0 || !data) {
            break;
        }
        
        // Decode the frame
        Av1DecodeOutput output;
        Av1DecodeResult decode_result = av1_decode(decoder, data, size, &output);
        
        if (decode_result == AV1_OK) {
            frames_decoded++;
            if (output.frame_ready) {
                frames_with_output++;
                printf("  Frame %d: frame_id=%u, ready=%d, show_existing=%d, dpb_slot=%d\n",
                       frames_decoded, output.frame_id, output.frame_ready,
                       output.show_existing_frame, output.dpb_slot);
            } else {
                printf("  Frame %d: decoded (not yet displayable)\n", frames_decoded);
            }
        } else if (decode_result == AV1_QUEUE_FULL) {
            queue_full_count++;
            printf("  Frame %d: QUEUE_FULL (ready queue is full)\n", frames_decoded + 1);
        } else {
            printf("  Frame %d: ERROR (code %d)\n", frames_decoded + 1, decode_result);
        }
        
        free(data);
        
        // Test queue full condition after queue_depth+1 frames
        if (frames_decoded == TEST_QUEUE_DEPTH + 1 && queue_full_count == 0) {
            printf("\n  NOTE: Queue depth is %d, but no QUEUE_FULL yet.\n", TEST_QUEUE_DEPTH);
            printf("  This is expected if frames are being consumed via av1_get_decoded_frame().\n");
        }
    }
    
    printf("\n--- Decode Summary ---\n");
    printf("Total frames decoded: %d\n", frames_decoded);
    printf("Frames with output:   %d\n", frames_with_output);
    printf("Queue full events:    %d\n", queue_full_count);
    
    // Test getting frames from queue
    printf("\n--- Getting Frames from Ready Queue ---\n");
    
    int frames_retrieved = 0;
    Av1FrameEntry entry;
    
    while (av1_get_decoded_frame(decoder, &entry, 0) == AV1_OK) {
        printf("  Retrieved: frame_id=%u, dpb_slot=%d, show=%d, show_existing=%d\n",
               entry.frame_id, entry.dpbslot, entry.show_frame, entry.show_existing_frame);
        
        // Release the frame
        av1_release_frame(decoder, entry.frame_id);
        frames_retrieved++;
    }
    
    printf("Frames retrieved from queue: %d\n", frames_retrieved);
    
    // Check queue status
    int queue_count = av1_frame_queue_count(&decoder->ready_queue);
    printf("Ready queue count after retrieval: %d\n", queue_count);
    
    // Test queue full by decoding without consuming
    printf("\n--- Testing QUEUE_FULL Condition ---\n");
    
    // Seek back to beginning
    ivf_parser_seek_frame(parser, 0);
    
    // Decode queue_depth+1 frames without consuming
    int frames_before_full = 0;
    for (int i = 0; i < TEST_QUEUE_DEPTH + 2; i++) {
        uint8_t *data = NULL;
        size_t size = 0;
        
        int result = ivf_parser_read_frame(parser, &data, &size, NULL);
        if (result != 0 || !data) {
            break;
        }
        
        Av1DecodeOutput output;
        Av1DecodeResult decode_result = av1_decode(decoder, data, size, &output);
        
        if (decode_result == AV1_QUEUE_FULL) {
            printf("  Queue became full after %d frames\n", frames_before_full);
            break;
        } else if (decode_result == AV1_OK && output.frame_ready) {
            frames_before_full++;
        }
        
        free(data);
    }
    
    // Consume all frames to drain the queue
    printf("\n--- Draining Queue ---\n");
    int drained = 0;
    while (av1_get_decoded_frame(decoder, &entry, 0) == AV1_OK) {
        av1_release_frame(decoder, entry.frame_id);
        drained++;
    }
    printf("Drained %d frames from queue\n", drained);
    
    // Get memory stats
    printf("\n--- Memory Statistics ---\n");
    Av1MemStats stats = av1_get_mem_stats(decoder);
    printf("  Total size:      %zu bytes (%.2f MB)\n", 
           stats.total_size, stats.total_size / (1024.0 * 1024.0));
    printf("  Used size:       %zu bytes (%.2f MB)\n", 
           stats.used_size, stats.used_size / (1024.0 * 1024.0));
    printf("  Peak usage:      %zu bytes (%.2f MB)\n", 
           stats.peak_usage, stats.peak_usage / (1024.0 * 1024.0));
    printf("  Allocations:     %zu\n", stats.num_allocations);
    printf("  Frees:           %zu\n", stats.num_frees);
    
    // Cleanup
    ivf_parser_close(parser);
    av1_destroy_decoder(decoder);
    free(mem_block);
    
    printf("\nDecode test completed!\n");
}

static void test_error_handling(void) {
    print_separator();
    printf("TEST: Error Handling\n");
    print_separator();
    
    // Query memory
    Av1StreamInfo info = {
        .width = 640,
        .height = 480,
        .max_bitrate = 8,
        .chroma_subsampling = 0,
        .is_16bit = false
    };
    
    Av1MemoryRequirements req = av1_query_memory(&info, 2, 1);
    
    void *mem = aligned_alloc(req.alignment, req.total_size);
    if (!mem) {
        printf("  Skipping error handling tests (memory allocation failed)\n");
        return;
    }
    memset(mem, 0, req.total_size);
    
    // Test 1: NULL decoder
    printf("\nTest 1: av1_decode with NULL decoder\n");
    uint8_t dummy_data[10] = {0};
    Av1DecodeOutput output;
    Av1DecodeResult result = av1_decode(NULL, dummy_data, sizeof(dummy_data), &output);
    if (result == AV1_INVALID_PARAM) {
        printf("  PASS: Correctly returned AV1_INVALID_PARAM\n");
    } else {
        printf("  FAIL: Expected AV1_INVALID_PARAM, got %d\n", result);
    }
    
    // Test 2: NULL data
    printf("\nTest 2: av1_decode with NULL data\n");
    Av1DecoderConfig config = {
        .memory_base = mem,
        .memory_size = req.total_size,
        .queue_depth = 2,
        .num_worker_threads = 1,
        .enable_threading = false
    };
    
    Av1Decoder *decoder = av1_create_decoder(&config);
    if (decoder) {
        result = av1_decode(decoder, NULL, 10, &output);
        if (result == AV1_INVALID_PARAM) {
            printf("  PASS: Correctly returned AV1_INVALID_PARAM\n");
        } else {
            printf("  FAIL: Expected AV1_INVALID_PARAM, got %d\n", result);
        }
        
        // Test 3: Zero size
        printf("\nTest 3: av1_decode with zero size\n");
        result = av1_decode(decoder, dummy_data, 0, &output);
        if (result == AV1_INVALID_PARAM) {
            printf("  PASS: Correctly returned AV1_INVALID_PARAM\n");
        } else {
            printf("  FAIL: Expected AV1_INVALID_PARAM, got %d\n", result);
        }
        
        // Test 4: Get state of NULL decoder
        printf("\nTest 4: av1_get_decoder_state with NULL\n");
        Av1DecoderState state = av1_get_decoder_state(NULL);
        if (state == AV1_DECODER_STATE_UNINITIALIZED) {
            printf("  PASS: Correctly returned UNINITIALIZED\n");
        } else {
            printf("  FAIL: Expected UNINITIALIZED, got %d\n", state);
        }
        
        // Test 5: Get decoded frame from empty queue
        printf("\nTest 5: av1_get_decoded_frame from empty queue\n");
        Av1FrameEntry entry;
        result = av1_get_decoded_frame(decoder, &entry, 0);  // Non-blocking
        if (result != AV1_OK) {
            printf("  PASS: Correctly returned error for empty queue\n");
        } else {
            printf("  FAIL: Should have returned error\n");
        }
        
        av1_destroy_decoder(decoder);
    }
    
    free(mem);
    printf("\nError handling tests completed!\n");
}

int main(int argc, char *argv[]) {
    printf("=== AV1 Decode Tests ===\n\n");
    
    // Default test file (can be overridden by command line)
    const char *test_file = "test.ivf";
    
    if (argc > 1) {
        test_file = argv[1];
    }
    
    // Test IVF parser first (just to verify file format)
    test_ivf_parser(test_file);
    
    // Test decode functionality
    test_decode_file(test_file);
    
    // Test error handling
    test_error_handling();
    
    print_separator();
    printf("All tests completed!\n");
    print_separator();
    
    return 0;
}
