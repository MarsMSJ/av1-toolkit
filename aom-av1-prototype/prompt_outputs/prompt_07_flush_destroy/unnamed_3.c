#include "av1_decoder_api.h"
#include "av1_mem_override.h"
#include "ivf_parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <unistd.h>

// Test configuration
#define TEST_WIDTH      640
#define TEST_HEIGHT     480
#define TEST_QUEUE_DEPTH 4
#define TEST_WORKERS    1

static void print_separator(void) {
    printf("============================================================\n");
}

// Allocate output buffer for YUV420
static Av1OutputBuffer* allocate_output_buffer(int width, int height, int bit_depth) {
    Av1OutputBuffer *buffer = (Av1OutputBuffer *)calloc(1, sizeof(Av1OutputBuffer));
    if (!buffer) {
        return NULL;
    }
    
    int bytes_per_pixel = (bit_depth > 8) ? 2 : 1;
    int y_size = width * height * bytes_per_pixel;
    int uv_size = (width / 2) * (height / 2) * bytes_per_pixel;
    
    buffer->planes[0] = (uint8_t *)aligned_alloc(64, y_size);
    buffer->planes[1] = (uint8_t *)aligned_alloc(64, uv_size);
    buffer->planes[2] = (uint8_t *)aligned_alloc(64, uv_size);
    
    if (!buffer->planes[0] || !buffer->planes[1] || !buffer->planes[2]) {
        free(buffer->planes[0]);
        free(buffer->planes[1]);
        free(buffer->planes[2]);
        free(buffer);
        return NULL;
    }
    
    buffer->strides[0] = width * bytes_per_pixel;
    buffer->strides[1] = (width / 2) * bytes_per_pixel;
    buffer->strides[2] = (width / 2) * bytes_per_pixel;
    
    buffer->widths[0] = width;
    buffer->widths[1] = width / 2;
    buffer->widths[2] = width / 2;
    buffer->heights[0] = height;
    buffer->heights[1] = height / 2;
    buffer->heights[2] = height / 2;
    
    buffer->width = width;
    buffer->height = height;
    buffer->bit_depth = bit_depth;
    buffer->chroma_subsampling = 0;  // 420
    
    return buffer;
}

static void free_output_buffer(Av1OutputBuffer *buffer) {
    if (!buffer) {
        return;
    }
    
    free(buffer->planes[0]);
    free(buffer->planes[1]);
    free(buffer->planes[2]);
    free(buffer);
}

// Test 1: Normal flush and destroy
// Decode N frames -> flush -> drain all -> destroy -> free memory
static void test_normal_flush_destroy(const char *ivf_file) {
    print_separator();
    printf("TEST 1: Normal Flush and Destroy\n");
    print_separator();
    
    // Query memory
    Av1StreamInfo info = {
        .width = TEST_WIDTH,
        .height = TEST_HEIGHT,
        .max_bitrate = 8,
        .chroma_subsampling = 0,
        .is_16bit = false
    };
    
    Av1MemoryRequirements req = av1_query_memory(&info, TEST_QUEUE_DEPTH, TEST_WORKERS);
    printf("Memory required: %zu bytes\n", req.total_size);
    
    // Allocate memory
    void *mem = aligned_alloc(req.alignment, req.total_size);
    if (!mem) {
        fprintf(stderr, "Failed to allocate memory\n");
        return;
    }
    memset(mem, 0, req.total_size);
    
    // Create decoder
    Av1DecoderConfig config = {
        .memory_base = mem,
        .memory_size = req.total_size,
        .queue_depth = TEST_QUEUE_DEPTH,
        .num_worker_threads = TEST_WORKERS,
        .worker_config = { .priority = 0, .cpu_affinity = -1, .core_id = -1 },
        .copy_thread_config = { .priority = 0, .cpu_affinity = -1, .core_id = -1 },
        .use_gpu = false,
        .gpu_device = 0,
        .gpu_thread_config = { .priority = 0, .cpu_affinity = -1, .core_id = -1 },
        .enable_threading = false,
        .enable_frame_parallel = false,
        .max_tile_cols = 0,
        .max_tile_rows = 0
    };
    
    Av1Decoder *decoder = av1_create_decoder(&config);
    if (!decoder) {
        fprintf(stderr, "Failed to create decoder\n");
        free(mem);
        return;
    }
    
    printf("Decoder created\n");
    
    // Open IVF file
    IvfParser *parser = ivf_parser_open(ivf_file);
    if (!parser) {
        fprintf(stderr, "Failed to open IVF file\n");
        av1_destroy_decoder(decoder);
        free(mem);
        return;
    }
    
    const IvfHeader *header = ivf_parser_get_header(parser);
    printf("Input: %s (%ux%u, %u frames)\n", 
           ivf_file, header->width, header->height, header->num_frames);
    
    // Allocate output buffer
    Av1OutputBuffer *output = allocate_output_buffer(header->width, header->height, 8);
    if (!output) {
        fprintf(stderr, "Failed to allocate output buffer\n");
        ivf_parser_close(parser);
        av1_destroy_decoder(decoder);
        free(mem);
        return;
    }
    
    // Decode frames
    printf("\n--- Decoding frames ---\n");
    int frames_decoded = 0;
    int max_frames = 10;  // Limit for test
    
    while (!ivf_parser_eof(parser) && frames_decoded < max_frames) {
        uint8_t *data = NULL;
        size_t size = 0;
        
        int result = ivf_parser_read_frame(parser, &data, &size, NULL);
        if (result != 0 || !data) {
            break;
        }
        
        Av1DecodeOutput decode_output;
        Av1DecodeResult decode_result = av1_decode(decoder, data, size, &decode_output);
        free(data);
        
        if (decode_result == AV1_OK && decode_output.frame_ready) {
            frames_decoded++;
            printf("  Decoded frame %d: frame_id=%u\n", frames_decoded, decode_output.frame_id);
        } else if (decode_result == AV1_QUEUE_FULL) {
            printf("  Queue full at frame %d\n", frames_decoded);
            break;
        }
    }
    
    printf("Decoded %d frames\n", frames_decoded);
    
    // Flush decoder
    printf("\n--- Flushing decoder ---\n");
    Av1DecodeResult flush_result = av1_flush(decoder);
    printf("Flush result: %d (AV1_OK=%d)\n", flush_result, AV1_OK);
    
    // Verify state is FLUSHING
    Av1DecoderState state = av1_get_decoder_state(decoder);
    printf("Decoder state after flush: %d (FLUSHING=%d)\n", state, AV1_DECODER_STATE_FLUSHING);
    
    // Try to decode more data (should be rejected)
    printf("\n--- Testing decode rejection after flush ---\n");
    uint8_t dummy_data[10] = {0};
    Av1DecodeOutput dummy_output;
    Av1DecodeResult reject_result = av1_decode(decoder, dummy_data, sizeof(dummy_data), &dummy_output);
    printf("Decode after flush result: %d (AV1_FLUSHED=%d)\n", reject_result, AV1_FLUSHED);
    
    // Drain remaining frames via sync -> set_output -> receive_output
    printf("\n--- Draining remaining frames ---\n");
    int frames_drained = 0;
    
    while (1) {
        Av1DecodeOutput sync_output;
        Av1DecodeResult sync_result = av1_sync(decoder, 100000, &sync_output);  // 100ms timeout
        
        if (sync_result == AV1_END_OF_STREAM) {
            printf("End of stream reached\n");
            break;
        }
        
        if (sync_result != AV1_OK) {
            printf("Sync returned %d, breaking\n", sync_result);
            break;
        }
        
        printf("  Synced frame_id=%u\n", sync_output.frame_id);
        
        // Set output and receive
        Av1DecodeResult set_result = av1_set_output(decoder, sync_output.frame_id, output);
        if (set_result == AV1_OK) {
            Av1DecodeResult receive_result = av1_receive_output(decoder, sync_output.frame_id, 1000000);
            if (receive_result == AV1_OK) {
                frames_drained++;
            }
        }
    }
    
    printf("Drained %d frames\n", frames_drained);
    
    // Destroy decoder
    printf("\n--- Destroying decoder ---\n");
    int destroy_result = av1_destroy_decoder(decoder);
    printf("Destroy result: %d\n", destroy_result);
    
    // Free memory
    printf("Freeing memory block\n");
    free(mem);
    free_output_buffer(output);
    ivf_parser_close(parser);
    
    printf("\nTEST 1 PASSED: Normal flush and destroy completed successfully\n");
}

// Test 2: Early destroy (without flush)
// Decode N frames -> destroy without flush -> no hang
static void test_early_destroy(const char *ivf_file) {
    print_separator();
    printf("TEST 2: Early Destroy (Without Flush)\n");
    print_separator();
    
    // Query memory
    Av1StreamInfo info = {
        .width = TEST_WIDTH,
        .height = TEST_HEIGHT,
        .max_bitrate = 8,
        .chroma_subsampling = 0,
        .is_16bit = false
    };
    
    Av1MemoryRequirements req = av1_query_memory(&info, TEST_QUEUE_DEPTH, TEST_WORKERS);
    
    // Allocate memory
    void *mem = aligned_alloc(req.alignment, req.total_size);
    if (!mem) {
        fprintf(stderr, "Failed to allocate memory\n");
        return;
    }
    memset(mem, 0, req.total_size);
    
    // Create decoder
    Av1DecoderConfig config = {
        .memory_base = mem,
        .memory_size = req.total_size,
        .queue_depth = TEST_QUEUE_DEPTH,
        .num_worker_threads = TEST_WORKERS,
        .enable_threading = false
    };
    
    Av1Decoder *decoder = av1_create_decoder(&config);
    if (!decoder) {
        fprintf(stderr, "Failed to create decoder\n");
        free(mem);
        return;
    }
    
    printf("Decoder created\n");
    
    // Open IVF file
    IvfParser *parser = ivf_parser_open(ivf_file);
    if (!parser) {
        av1_destroy_decoder(decoder);
        free(mem);
        return;
    }
    
    // Decode only 5 frames
    printf("\n--- Decoding 5 frames ---\n");
    int frames_decoded = 0;
    
    while (!ivf_parser_eof(parser) && frames_decoded < 5) {
        uint8_t *data = NULL;
        size_t size = 0;
        
        int result = ivf_parser_read_frame(parser, &data, &size, NULL);
        if (result != 0 || !data) break;
        
        Av1DecodeOutput decode_output;
        av1_decode(decoder, data, size, &decode_output);
        free(data);
        
        if (decode_output.frame_ready) {
            frames_decoded++;
            printf("  Decoded frame %d\n", frames_decoded);
        }
    }
    
    printf("Decoded %d frames\n", frames_decoded);
    
    // Check ready queue count
    int queue_count = av1_frame_queue_count(&decoder->ready_queue);
    printf("Ready queue has %d frames\n", queue_count);
    
    // Destroy WITHOUT calling flush first
    printf("\n--- Destroying decoder WITHOUT flush ---\n");
    int destroy_result = av1_destroy_decoder(decoder);
    printf("Destroy result: %d\n", destroy_result);
    
    // Free memory
    free(mem);
    ivf_parser_close(parser);
    
    printf("\nTEST 2 PASSED: Early destroy completed without hang\n");
}

// Test 3: Empty destroy (create -> destroy immediately)
static void test_empty_destroy(void) {
    print_separator();
    printf("TEST 3: Empty Destroy (Create -> Destroy Immediately)\n");
    print_separator();
    
    // Query memory
    Av1StreamInfo info = {
        .width = 320,
        .height = 240,
        .max_bitrate = 8,
        .chroma_subsampling = 0,
        .is_16bit = false
    };
    
    Av1MemoryRequirements req = av1_query_memory(&info,

