#include "av1_decoder_api.h"
#include "av1_mem_override.h"
#include "ivf_parser.h"
#include "y4m_writer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <sys/stat.h>

// Test configuration
#define TEST_WIDTH      1920
#define TEST_HEIGHT     1080
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

// Get file size
static size_t get_file_size(const char *filename) {
    struct stat st;
    if (stat(filename, &st) != 0) {
        return 0;
    }
    return st.st_size;
}

// Test 1: Basic sync -> set_output -> receive_output flow
static void test_basic_sync_output(const char *ivf_file, const char *y4m_file) {
    print_separator();
    printf("TEST: Basic Sync/Output Flow\n");
    print_separator();
    
    // Query memory
    Av1StreamInfo info = {
        .width = TEST_WIDTH,
        .height = TEST_HEIGHT,
        .max_bitrate = 10,
        .chroma_subsampling = 0,
        .is_16bit = false
    };
    
    Av1MemoryRequirements req = av1_query_memory(&info, TEST_QUEUE_DEPTH, TEST_WORKERS);
    printf("Memory required: %zu bytes (%.2f MB)\n", 
           req.total_size, req.total_size / (1024.0 * 1024.0));
    
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
    
    printf("Decoder created successfully\n");
    
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
    
    // Open Y4M writer
    Y4MWriter *y4m = y4m_writer_open(y4m_file, 
                                       header->width, 
                                       header->height,
                                       header->timebase_den > 0 ? header->timebase_den : 30,
                                       header->timebase_num > 0 ? header->timebase_num : 1,
                                       8,  // bit depth
                                       0); // chroma subsampling (420)
    if (!y4m) {
        fprintf(stderr, "Failed to create Y4M writer\n");
        ivf_parser_close(parser);
        av1_destroy_decoder(decoder);
        free(mem);
        return;
    }
    
    printf("Output: %s\n", y4m_file);
    
    // Allocate output buffer
    Av1OutputBuffer *output = allocate_output_buffer(header->width, header->height, 8);
    if (!output) {
        fprintf(stderr, "Failed to allocate output buffer\n");
        y4m_writer_close(y4m);
        ivf_parser_close(parser);
        av1_destroy_decoder(decoder);
        free(mem);
        return;
    }
    
    // Decode and output frames
    printf("\n--- Decoding and Outputting Frames ---\n");
    
    int frames_decoded = 0;
    int frames_output = 0;
    int sync_calls = 0;
    int set_output_calls = 0;
    int receive_output_calls = 0;
    
    while (!ivf_parser_eof(parser)) {
        uint8_t *data = NULL;
        size_t size = 0;
        
        int result = ivf_parser_read_frame(parser, &data, &size, NULL);
        if (result != 0 || !data) {
            break;
        }
        
        // Decode frame
        Av1DecodeOutput decode_output;
        Av1DecodeResult decode_result = av1_decode(decoder, data, size, &decode_output);
        free(data);
        
        if (decode_result != AV1_OK) {
            printf("Decode error at frame %d\n", frames_decoded);
            continue;
        }
        
        frames_decoded++;
        
        // If frame is ready, process it through sync -> set_output -> receive_output
        if (decode_output.frame_ready) {
            printf("Frame %d: frame_id=%u ready\n", frames_decoded, decode_output.frame_id);
            
            // Step 1: av1_sync - get frame from ready queue
            Av1DecodeOutput sync_output;
            Av1DecodeResult sync_result = av1_sync(decoder, 0, &sync_output);
            sync_calls++;
            
            if (sync_result == AV1_OK) {
                printf("  sync: frame_id=%u\n", sync_output.frame_id);
                
                // Step 2: av1_set_output - copy to output buffer
                Av1DecodeResult set_result = av1_set_output(decoder, sync_output.frame_id, output);
                set_output_calls++;
                
                if (set_result == AV1_OK) {
                    printf("  set_output: enqueued copy job\n");
                    
                    // Step 3: av1_receive_output - wait for copy and release
                    Av1DecodeResult receive

Av1DecodeResult receive_result = av1_receive_output(decoder, sync_output.frame_id, 0);
                    receive_output_calls++;
                    
                    if (receive_result == AV1_OK) {
                        printf("  receive_output: completed\n");
                        
                        // Write frame to Y4M
                        if (y4m_writer_write_buffer(y4m, output) == 0) {
                            frames_output++;
                        } else {
                            printf("  ERROR: failed to write Y4M frame\n");
                        }
                    } else {
                        printf("  ERROR: receive_output failed\n");
                    }
                } else {
                    printf("  ERROR: set_output failed\n");
                }
            } else if (sync_result == AV1_NEED_MORE_DATA) {
                printf("  sync: NEED_MORE_DATA (unexpected)\n");
            } else if (sync_result == AV1_END_OF_STREAM) {
                printf("  sync: END_OF_STREAM\n");
                break;
            }
        }
    }
    
    printf("\n--- Decode/Output Summary ---\n");
    printf("Frames decoded:    %d\n", frames_decoded);
    printf("Frames output:     %d\n", frames_output);
    printf("sync() calls:      %d\n", sync_calls);
    printf("set_output() calls: %d\n", set_output_calls);
    printf("receive_output() calls: %d\n", receive_output_calls);
    
    // Signal end of stream
    printf("\n--- Signaling End of Stream ---\n");
    av1_decode_end(decoder);
    
    // Try to sync any remaining frames
    int remaining = 0;
    while (1) {
        Av1DecodeOutput sync_output;
        Av1DecodeResult sync_result = av1_sync(decoder, 100000, &sync_output);  // 100ms timeout
        
        if (sync_result == AV1_END_OF_STREAM) {
            printf("End of stream reached\n");
            break;
        }
        
        if (sync_result == AV1_NEED_MORE_DATA) {
            remaining++;
            if (remaining > 10) {
                printf("Timeout waiting for frames, breaking\n");
                break;
            }
            continue;
        }
        
        if (sync_result == AV1_OK) {
            printf("Remaining frame: frame_id=%u\n", sync_output.frame_id);
            
            // Process this frame too
            Av1DecodeResult set_result = av1_set_output(decoder, sync_output.frame_id, output);
            if (set_result == AV1_OK) {
                Av1DecodeResult receive_result = av1_receive_output(decoder, sync_output.frame_id, 0);
                if (receive_result == AV1_OK) {
                    y4m_writer_write_buffer(y4m, output);
                    frames_output++;
                }
            }
        }
    }
    
    printf("\n--- Final Summary ---\n");
    printf("Total frames output: %d\n", frames_output);
    
    // Cleanup
    free_output_buffer(output);
    y4m_writer_close(y4m);
    ivf_parser_close(parser);
    av1_destroy_decoder(decoder);
    free(mem);
    
    printf("\nBasic sync/output test completed!\n");
}

// Test 2: Test queue full handling
static void test_queue_full_handling(const char *ivf_file) {
    print_separator();
    printf("TEST: Queue Full Handling\n");
    print_separator();
    
    // Use small queue depth to trigger queue full
    const int queue_depth = 2;
    
    Av1StreamInfo info = {
        .width = 640,
        .height = 480,
        .max_bitrate = 8,
        .chroma_subsampling = 0,
        .is_16bit = false
    };
    
    Av1MemoryRequirements req = av1_query_memory(&info, queue_depth, 1);
    
    void *mem = aligned_alloc(req.alignment, req.total_size);
    if (!mem) {
        fprintf(stderr, "Failed to allocate memory\n");
        return;
    }
    memset(mem, 0, req.total_size);
    
    Av1DecoderConfig config = {
        .memory_base = mem,
        .memory_size = req.total_size,
        .queue_depth = queue_depth,
        .num_worker_threads = 1,
        .enable_threading = false
    };
    
    Av1Decoder *decoder = av1_create_decoder(&config);
    if (!decoder) {
        free(mem);
        return;
    }
    
    // Open IVF
    IvfParser *parser = ivf_parser_open(ivf_file);
    if (!parser) {
        av1_destroy_decoder(decoder);
        free(mem);
        return;
    }
    
    printf("Decoding with queue_depth=%d\n", queue_depth);
    
    // Decode frames without consuming
    int frames_decoded = 0;
    int queue_full_count = 0;
    
    while (!ivf_parser_eof(parser)) {
        uint8_t *data = NULL;
        size_t size = 0;
        
        int result = ivf_parser_read_frame(parser, &data, &size, NULL);
        if (result != 0 || !data) break;
        
        Av1DecodeOutput output;
        Av1DecodeResult decode_result = av1_decode(decoder, data, size, &output);
        free(data);
        
        if (decode_result == AV1_QUEUE_FULL) {
            queue_full_count++;
            printf("  Queue full at frame %d\n", frames_decoded);
            break;
        } else if (decode_result == AV1_OK && output.frame_ready) {
            frames_decoded++;
        }
    }
    
    printf("Decoded %d frames before queue full (count=%d)\n", frames_decoded, queue_full_count);
    
    // Now consume frames and decode more
    printf("\n--- Consuming frames ---\n");
    
    Av1OutputBuffer *output = allocate_output_buffer(640, 480, 8);
    int consumed = 0;
    
    while (av1_frame_queue_count(&decoder->ready_queue) > 0) {
        Av1DecodeOutput sync_output;
        if (av1_sync(decoder, 0, &sync_output) == AV1_OK) {
            av1_set_output(decoder, sync_output.frame_id, output);
            av1_receive_output(decoder, sync_output.frame_id, 0);
            consumed++;
        }
    }
    
    printf("Consumed %d frames\n", consumed);
    
    // Continue decoding
    int more_decoded = 0;
    while (!ivf_parser_eof(parser)) {
        uint8_t *data = NULL;
        size_t size = 0;
        
        int result = ivf_parser_read_frame(parser, &data, &size, NULL);
        if (result != 0 || !data) break;
        
        Av1DecodeOutput output_dec;
        Av1DecodeResult decode_result = av1_decode(decoder, data, size, &output_dec);
        free(data);
        
        if (decode_result == AV1_OK && output_dec.frame_ready) {
            more_decoded++;
        } else if (decode_result == AV1_QUEUE_FULL) {
            printf("Queue full again after consuming\n");
            break;
        }
    }
    
    printf("Decoded %d more frames after consuming\n", more_decoded);
    
    free_output_buffer(output);
    ivf_parser_close(parser);
    av1_destroy_decoder(decoder);
    free(mem);
    
    printf("\nQueue full handling test completed!\n");
}

// Test 3: Error handling
static void test_error_handling(void) {
    print_separator();
    printf("TEST: Error Handling\n");
    print_separator();
    
    // Query memory
    Av1StreamInfo info = { .width = 320, .height = 240, .max_bitrate = 8 };
    Av1MemoryRequirements req = av1_query_memory(&info, 2, 1);
    
    void *mem = aligned_alloc(req.alignment, req.total_size);
    if (!mem) {
        printf("  Skipping (memory allocation failed)\n");
        return;
    }
    memset(mem, 0, req.total_size);
    
    // Test 1: av1_sync with NULL decoder
    printf("\nTest 1: av1_sync with NULL decoder\n");
    Av1DecodeOutput output;
    Av1DecodeResult result = av1_sync(NULL, 0, &output);
    if (result == AV1_INVALID_PARAM) {
        printf("  PASS: AV1_INVALID_PARAM\n");
    } else {
        printf("  FAIL: got %d\n", result);
    }
    
    // Test 2: av1_set_output with NULL decoder
    printf("\nTest 2: av1_set_output with NULL decoder\n");
    Av1OutputBuffer buf = {0};
    result = av1_set_output(NULL, 0, &buf);
    if (result == AV1_INVALID_PARAM) {
        printf("  PASS: AV1_INVALID_PARAM\n");
    } else {
        printf("  FAIL: got %d\n", result);
    }
    
    // Test 3: av1_receive_output with NULL decoder
    printf("\nTest 3: av1_receive_output with NULL decoder\n");
    result = av1_receive_output(NULL, 0, 0);
    if (result == AV1_INVALID_PARAM) {
        printf("  PASS: AV1_INVALID_PARAM\n");
    } else {
        printf("  FAIL: got %d\n", result);
    }
    
    // Test 4: av1_set_output with invalid frame_id
    printf("\nTest 4: av1_set_output with invalid frame_id\n");
    Av1DecoderConfig config = {
        .memory_base = mem,
        .memory_size = req.total_size,
        .queue_depth = 2,
        .num_worker_threads = 1,
        .enable_threading = false
    };
    
    Av1Decoder *decoder = av1_create_decoder(&config);
    if (decoder) {
        result = av1_set_output(decoder, 9999, &buf);  // Non-existent frame
        if (result == AV1_ERROR) {
            printf("  PASS: AV1_ERROR\n");
        } else {
            printf("  FAIL: got %d\n", result);
        }
        
        // Test 5: av1_receive_output with invalid frame_id
        printf("\nTest 5: av1_receive_output with invalid frame_id\n");
        result = av1_receive_output(decoder, 9999, 0);
        if (result == AV1_ERROR) {
            printf("  PASS: AV1_ERROR\n");
        } else {
            printf("  FAIL: got %d\n", result);
        }
        
        // Test 6: av1_sync timeout
        printf("\nTest 6: av1_sync timeout on empty queue\n");
        result = av1_sync(decoder, 10000, &output);  // 10ms timeout
        if (result == AV1_NEED_MORE_DATA) {
            printf("  PASS: AV1_NEED_MORE_DATA\n");
        } else {
            printf("  FAIL: got %d\n", result);
        }
        
        av1_destroy_decoder(decoder);
    }
    
    free(mem);
    printf("\nError handling tests completed!\n");
}

// Test 4: End of stream handling
static void test_end_of_stream(const char *ivf_file) {
    print_separator();
    printf("TEST: End of Stream\n");
    print_separator();
    
    Av1StreamInfo info = { .width = 640, .height = 480, .max_bitrate = 8 };
    Av1MemoryRequirements req = av1_query_memory(&info, 2, 1);
    
    void *mem = aligned_alloc(req.alignment, req.total_size);
    if (!mem) return;
    memset(mem, 0, req.total_size);
    
    Av1DecoderConfig config = {
        .memory_base = mem,
        .memory_size = req.total_size,
        .queue_depth = 2,
        .num_worker_threads = 1,
        .enable_threading = false
    };
    
    Av1Decoder *decoder = av1_create_decoder(&config);
    if (!decoder) {
        free(mem);
        return;
    }
    
    IvfParser *parser = ivf_parser_open(ivf_file);
    if (!parser) {
        av1_destroy_decoder(decoder);
        free(mem);
        return;
    }
    
    // Decode all frames
    while (!ivf_parser_eof(parser)) {
        uint8_t *data = NULL;
        size_t size = 0;
        
        if (ivf_parser_read_frame(parser, &data, &size, NULL) != 0 || !data) break;
        
        av1_decode(decoder, data, size, NULL);
        free(data);
    }
    
    printf("Decoded all %d frames from file\n", ivf_parser_get_frame_index(parser));
    
    // Signal end of stream
    printf("Calling av1_decode_end()\n");
    av1_decode_end(decoder);
    
    // Try to sync remaining frames
    printf("\n--- Syncing remaining frames ---\n");
    int synced = 0;
    
    while (1) {
        Av1DecodeOutput output;
        Av1DecodeResult result = av1_sync(decoder, 100000, &output);  // 100ms
        
        if (result == AV1_END_OF_STREAM) {
            printf("Got AV1_END_OF_STREAM after %d frames\n", synced);
            break;
        }
        
        if (result == AV1_OK) {
            synced++;
            printf("  Synced frame_id=%u\n", output.frame_id);
            
            // Release it
            av1_release_frame(decoder, output.frame_id);
        } else if (result == AV1_NEED_MORE_DATA) {
            printf("Got NEED_MORE_DATA, waiting more...\n");
            // Give it a moment
            struct timespec ts = {0, 10000000};  // 10ms
            nanosleep(&ts, NULL);
        }
    }
    
    ivf_parser_close(parser);
    av1_destroy_decoder(decoder);
    free(mem);
    
    printf("\nEnd of stream test completed!\n");
}

int main(int argc, char *argv[]) {
    printf("=== AV1 Sync/Output Tests ===\n\n");
    
    const char *ivf_file = "test.ivf";
    const char *y4m_file = "output.y4m";
    
    if (argc > 1) {
        ivf_file = argv[1];
    }
    if (argc > 2) {
        y4m_file = argv[2];
    }
    
    // Check if IVF file exists
    if (!ivf_parser_is_valid(ivf_file)) {
        fprintf(stderr, "Error: %s is not a valid IVF file\n", ivf_file);
        fprintf(stderr, "Usage: %s [input.ivf] [output.y4m]\n", argv[0]);
        return 1;
    }
    
    // Run tests
    test_basic_sync_output(ivf_file, y4m_file);
    printf("\n");
    
    test_queue_full_handling(ivf_file);
    printf("\n");
    
    test_error_handling();
    printf("\n");
    
    test_end_of_stream(ivf_file);
    printf("\n");
    
    print_separator();
    printf("All tests completed!\n");
    print_separator();
    
    return 0;
}
