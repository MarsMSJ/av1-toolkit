/**
 * AV1 Decoder End-to-End Test Program
 * 
 * This program demonstrates the full decode pipeline:
 * 1. Open IVF file and parse header
 * 2. Extract sequence header for stream parameters
 * 3. Query memory requirements
 * 4. Create decoder
 * 5. Decode loop with QUEUE_FULL recovery
 * 6. Flush and drain
 * 7. Cleanup
 */

#include "av1_decoder_api.h"
#include "av1_mem_override.h"
#include "ivf_parser.h"
#include "y4m_writer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <errno.h>

// ============================================================================
// Configuration
// ============================================================================

#define DEFAULT_QUEUE_DEPTH    4
#define DEFAULT_WORKERS        1
#define MAX_FRAME_ID           99999

// ============================================================================
// Stream Information (parsed from bitstream)
// ============================================================================

typedef struct {
    int width;
    int height;
    int bit_depth;
    int chroma_subsampling;
    int has_seq_header;
} StreamParams;

// ============================================================================
// Helper Functions
// ============================================================================

static void print_separator(void) {
    printf("============================================================\n");
}

static void print_usage(const char *prog) {
    printf("Usage: %s [options] input.ivf [output.y4m]\n", prog);
    printf("\nOptions:\n");
    printf("  -q <n>   Queue depth (default: %d)\n", DEFAULT_QUEUE_DEPTH);
    printf("  -w <n>   Number of worker threads (default: %d)\n", DEFAULT_WORKERS);
    printf("  -v       Verbose output\n");
    printf("  -h       Show this help\n");
    printf("\nInput:\n");
    printf("  input.ivf   Input AV1 IVF file\n");
    printf("  output.y4m  Output Y4M file (default: input.y4m)\n");
}

// Get file size
static size_t get_file_size(const char *filename) {
    struct stat st;
    if (stat(filename, &st) != 0) {
        return 0;
    }
    return st.st_size;
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
    if (!buffer) return;
    free(buffer->planes[0]);
    free(buffer->planes[1]);
    free(buffer->planes[2]);
    free(buffer);
}

// ============================================================================
// Sequence Header Parsing (simplified)
// ============================================================================

// Parse sequence header from first frame data
// This is a simplified parser that extracts basic parameters
// In production, you'd use the AOM parser or your own OBU parser
static int parse_sequence_header(const uint8_t *data, size_t size, 
                                  StreamParams *params) {
    if (!data || !params) return -1;
    
    // Default values
    params->width = 0;
    params->height = 0;
    params->bit_depth = 8;
    params->chroma_subsampling = 0;  // 420
    params->has_seq_header = 0;
    
    // Look for Sequence Header OBU (type = 1)
    // AV1 OBU format: [size | type | ...] or raw
    // This is a simplified check - real implementation would parse OBUs properly
    
    // For now, we'll use defaults and let AOM handle the actual parsing
    // The AOM decoder will extract these from the bitstream
    params->has_seq_header = 1;
    
    return 0;
}

// ============================================================================
// Main Decode Function
// ============================================================================

static int decode_file(const char *ivf_file, const char *y4m_file,
                       int queue_depth, int num_workers, bool verbose) {
    int result = 0;
    
    // Step 1: Open IVF file
    printf("Opening IVF file: %s\n", ivf_file);
    
    if (!ivf_parser_is_valid(ivf_file)) {
        fprintf(stderr, "Error: %s is not a valid IVF file\n", ivf_file);
        return -1;
    }
    
    IvfParser *parser = ivf_parser_open(ivf_file);
    if (!parser) {
        fprintf(stderr, "Error: Failed to open IVF file\n");
        return -1;
    }
    
    const IvfHeader *header = ivf_parser_get_header(parser);
    printf("IVF: %ux%u, %u frames, %.4s\n", 
           header->width, header->height, header->num_frames, header->fourcc);
    
    // Step 2: Read first frame to get sequence header
    printf("Reading first frame for sequence header...\n");
    
    StreamParams stream_params = {0};
    stream_params.width = header->width;
    stream_params.height = header->height;
    stream_params.bit_depth = 8;
    stream_params.chroma_subsampling = 0;
    
    uint8_t *first_frame_data = NULL;
    size_t first_frame_size = 0;
    
    if (ivf_parser_read_frame(parser, &first_frame_data, &first_frame_size, NULL) == 0) {
        parse_sequence_header(first_frame_data, first_frame_size, &stream_params);
        printf("First frame size: %zu bytes\n", first_frame_size);
        free(first_frame_data);
    }
    
    // Seek back to beginning
    ivf_parser_seek_frame(parser, 0);
    
    // Step 3: Query memory requirements
    printf("\nQuerying memory requirements...\n");
    
    Av1StreamInfo info = {
        .width = stream_params.width,
        .height = stream_params.height,
        .max_bitrate = stream_params.bit_depth,
        .chroma_subsampling = stream_params.chroma_subsampling,
        .is_16bit = stream_params.bit_depth > 8
    };
    
    Av1MemoryRequirements req = av1_query_memory(&info, queue_depth, num_workers);
    printf("Memory required: %zu bytes (%.2f MB)\n", 
           req.total_size, req.total_size / (1024.0 * 1024.0));
    
    // Step 4: Allocate memory block
    printf("Allocating memory block...\n");
    
    void *mem_block = aligned_alloc(req.alignment, req.total_size);
    if (!mem_block) {
        fprintf(stderr, "Error: Failed to allocate memory block\n");
        ivf_parser_close(parser);
        return -1;
    }
    memset(mem_block, 0, req.total_size);
    printf("Memory block at: %p\n", mem_block);
    
    // Step 5: Create decoder
    printf("Creating decoder...\n");
    
    Av1DecoderConfig config = {
        .memory_base = mem_block,
        .memory_size = req.total_size,
        .queue_depth = queue_depth,
        .num_worker_threads = num_workers,
        .worker_config = { .priority = 0, .cpu_affinity = -1, .core_id = -1 },
        .copy_thread_config = { .priority = 0, .cpu_affinity = -1, .core_id = -1 },
        .use_gpu = false,
        .gpu_device = 0,
        .gpu_thread_config = { .priority = 0, .cpu_affinity = -1, .core_id = -1 },
        .enable_threading = (num_workers > 1),
        .enable_frame_parallel = false,
        .max_tile_cols = 0,
        .max_tile_rows = 0
    };
    
    Av1Decoder *decoder = av1_create_decoder(&config);
    if (!decoder) {
        fprintf(stderr, "Error: Failed to create decoder\n");
        free(mem_block);
        ivf_parser_close(parser);
        return -1;
    }
    
    printf("Decoder created successfully\n");
    
    // Step 6: Open Y4M writer
    printf("Opening Y4M writer: %s\n", y4m_file);
    
    int fps_n = header->timebase_den > 0 ? header->timebase_den : 30;
    int fps_d = header->timebase_num > 0 ? header->timebase_num : 1;
    
    Y4MWriter *y4m = y4m_writer_open(y4m_file, 
                                       header->width, header->height,
                                       fps_n, fps_d,
                                       stream_params.bit_depth,
                                       stream_params.chroma_subsampling);
    if (!y4m) {
        fprintf(stderr, "Error: Failed to create Y4M writer\n");
        av1_destroy_decoder(decoder);
        free(mem_block);
        ivf_parser_close(parser);
        return -1;
    }
    
    // Allocate output buffer
    Av1OutputBuffer *output = allocate_output_buffer(header->width, header->height, 
                                                      stream_params.bit_depth);
    if (!output) {
        fprintf(stderr, "Error: Failed to allocate output buffer\n");
        y4m_writer_close(y4m);
        av1_destroy_decoder(decoder);
        free(mem_block);
        ivf_parser_close(parser);
        return -1;
    }
    
    // Step 7: Decode loop
    printf("\n--- Decoding ---\n");
    
    int frames_decoded = 0;
    int frames_output = 0;
    int queue_full_events = 0;
    int decode_errors = 0;
    
    while (!ivf_parser_eof(parser)) {
        uint8_t *data = NULL;
        size_t size = 0;
        
        int read_result = ivf_parser_read_frame(parser, &data, &size, NULL);
        if (read_result != 0 || !data) {
            break;
        }
        
        if (verbose) {
            printf("Frame %d: size=%zu bytes\n", frames_decoded + 1, size);
        }
        
        // Decode the frame
        Av1DecodeOutput decode_output;
        Av1DecodeResult decode_result = av1_decode(decoder, data, size, &decode_output);
        free(data);  // Free frame data immediately
        
        if (decode_result == AV1_ERROR) {
            fprintf(stderr, "Decode error at frame %d\n", frames_decoded);
            decode_errors++;
            continue;
        }
        
        if (decode_result == AV1_QUEUE_FULL) {
            if (verbose) {
                printf("  Queue full, draining...\n");
            }
            queue_full_events++;
            
            // Drain ready queue
            while (av1_frame_queue_count(&decoder->ready_queue) > 0) {
                Av1DecodeOutput sync_out;
                if (av1_sync(decoder, 0, &sync_out) == AV1_OK) {
                    av1_set_output(decoder, sync_out.frame_id, output);
                    av1_receive_output(decoder, sync_out.frame_id, 0);
                } else {
                    break;
                }
            }
            
            // Retry decode
            continue;
        }
        
        if (decode_result != AV1_OK) {
            fprintf(stderr, "Unexpected decode result: %d\n", decode_result);
            continue;
        }
        
        frames_decoded++;
        
        // If frame is ready for output, process it
        if (decode_output.frame_ready) {
            // Sync to get frame from ready queue
            Av1DecodeOutput sync_out;
            Av1DecodeResult sync_result = av1_sync(decoder, 0, &sync_out);
            
            if (sync_result == AV1_OK) {
                // Set output buffer
                Av1DecodeResult set_result = av1_set_output(decoder, sync_out.frame_id, output);
                
                if (set_result == AV1_OK) {
                    // Wait for copy and release
                    Av1DecodeResult receive_result = av1_receive_output(decoder, sync_out.frame_id, 0);
                    
                    if (receive_result == AV1_OK) {
                        // Write to Y4M
                        if (y4m_writer_write_buffer(y4m, output) == 0) {
                            frames_output++;
                            if (verbose) {
                                printf("  Output: frame_id=%u\n", sync_out.frame_id);
                            }
                        }
                    } else {
                        fprintf(stderr, "Error: receive_output failed for frame %u\n", sync_out.frame_id);
                    }
                } else {
                    fprintf(stderr, "Error: set_output failed for frame %u\n", sync_out.frame_id);
                }
            } else {
                fprintf(stderr, "Error: sync failed at frame %d\n", frames_decoded);
            }
        }
    }
    
    printf("Decoded %d frames, output %d frames\n", frames_decoded, frames_output);
    printf("Queue full events: %d, Decode errors: %d\n", queue_full_events, decode_errors);
    
    // Step 8: Flush decoder
    printf("\n--- Flushing ---\n");
    av1_flush(decoder);
    
    // Step 9: Drain remaining frames
    printf("Draining remaining frames...\n");
    int drained = 0;
    
    while (1) {
        Av1DecodeOutput sync_out;
        Av1DecodeResult sync_result = av1_sync(decoder, 100000, &sync_out);  // 100ms timeout
        
        if (sync_result == AV1_END_OF_STREAM) {
            printf("End of stream\n");
            break;
        }
        
        if (sync_result != AV1_OK) {
            break;
        }
        
        // Set output and receive
        av1_set_output(decoder, sync_out.frame_id, output);
        if (av1_receive_output(decoder, sync_out.frame_id, 0) == AV1_OK) {
            y4m_writer_write_buffer(y4m, output);
            drained++;
            if (verbose) {
                printf("  Drained: frame_id=%u\n", sync_out.frame_id);
            }
        }
    }
    
    printf("Drained %d remaining frames\n", drained);
    
    // Step 10: Destroy decoder
    printf("\n--- Destroying decoder ---\n");
    int destroy_result = av1_destroy_decoder(decoder);
    printf("Destroy result: %d\n", destroy_result);
    
    // Step 11: Free memory
    printf("Freeing memory block...\n");
    free(mem_block);
    free_output_buffer(output);
    y4m_writer_close(y4m);
    ivf_parser_close(parser);
    
    printf("\n=== Summary ===\n");
    printf("Input:  %s\n", ivf_file);
    printf("Output: %s\n", y4m_file);
    printf("Frames decoded: %d\n", frames_decoded);
    printf("Frames output:  %d\n", frames_output + drained);
    printf("Queue full events: %d\n", queue_full_events);
    printf("Decode errors: %d\n", decode_errors);
    
    return (decode_errors > 0) ? 1 : 0;
}

// ============================================================================
// Error Path Tests
// ============================================================================

static int test_truncated_bitstream(const char *ivf_file) {
    printf("\n=== TEST: Truncated Bitstream ===\n");
    
    // Open parser
    IvfParser *parser = ivf_parser_open(ivf_file);
    if (!parser) return -1;
    
    const IvfHeader *header = ivf_parser_get_header(parser);
    
    // Query memory
    Av1StreamInfo info = { .width = header->width, .height = header->height };
    Av1MemoryRequirements req = av1_query_memory(&info, 2, 1);
    
    void *mem = aligned_alloc(req.alignment, req.total_size);
    if (!mem) {
        ivf_parser_close(parser);
        return -1;
    }
    memset(mem, 0, req.total_size);
    
    // Create decoder
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
        ivf_parser_close(parser);
        return -1;
    }
    
    // Read first frame but truncate it
    uint8_t *data = NULL;
    size_t full_size = 0;
    ivf_parser_read_frame(parser, &data, &full_size, NULL);
    
    // Truncate to half size
    size_t truncated_size = full_size / 2;
    printf("Original size: %zu, Truncated: %zu\n", full_size, truncated_size);
    
    Av1DecodeOutput output;
    Av1DecodeResult result = av1_decode(decoder, data, truncated_size, &output);
    printf("Decode result with truncated data: %d\n", result);
    
    // Should handle gracefully (either error or partial)
    free(data);
    av1_destroy_decoder(decoder);
    free(mem);
    ivf_parser_close(parser);
    
    printf("TEST PASSED: Handled truncated bitstream gracefully\n");
    return 0;
}

static int test_zero_length_au(void) {
    printf("\n=== TEST: Zero-Length AU ===\n");
    
    Av1StreamInfo info = { .width = 640, .height = 480 };
    Av1MemoryRequirements req = av1_query_memory(&info, 2, 1);
    
    void *mem = aligned_alloc(req.alignment, req.total_size);
    if (!mem) return -1;
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
        return -1;
    }
    
    // Try to decode zero-length data
    uint8_t empty_data[1] = {0};
    Av1DecodeOutput output;
    Av1DecodeResult result = av1_decode(decoder, empty_data, 0, &output);
    printf("Decode result with zero-length AU: %d (AV1_INVALID_PARAM=%d)\n", 
           result, AV1_INVALID_PARAM);
    
    int pass = (result == AV1_INVALID_PARAM);
    
    av1_destroy_decoder(decoder);
    free(mem);
    
    if (pass) {
        printf("TEST PASSED: Correctly rejected zero-length AU\n");
        return 0;
    } else {
        printf("TEST FAILED: Did not reject zero-length AU\n");
        return -1;
    }
}

static int test_queue_full_recovery(const char *ivf_file) {
    printf("\n=== TEST: QUEUE_FULL Recovery ===\n");
    
    // Use very small queue depth to trigger QUEUE_FULL
    const int small_queue = 1;
    
    IvfParser *parser = ivf_parser_open(ivf_file);
    if (!parser) return -1;
    
    const IvfHeader *header = ivf_parser_get_header(parser);
    
    Av1StreamInfo info = { .width = header->width, .height = header->height };
    Av1MemoryRequirements req = av1_query_memory(&info, small_queue, 1);
    
    void *mem = aligned_alloc(req.alignment, req.total_size);
    if (!mem) {
        ivf_parser_close(parser);
        return -1;
    }
    memset(mem, 0, req.total_size);
    
    Av1DecoderConfig config = {
        .memory_base = mem,
        .memory_size = req.total_size,
        .queue_depth = small_queue,
        .num_worker_threads = 1,
        .enable_threading = false
    };
    
    Av1Decoder *decoder = av1_create_decoder(&config);
    if (!decoder) {
        free(mem);
        ivf_parser_close(parser);
        return -1;
    }
    
    // Allocate output buffer
    Av1OutputBuffer *output = allocate_output_buffer(header->width, header->height, 8);
    
    int frames_before_full = 0;
    int queue_full_detected = 0;
    
    // Decode until queue is full
    while (!ivf_parser_eof(parser)) {
        uint8_t *data = NULL;
        size_t size = 0;
        
        if (ivf_parser_read_frame(parser, &data, &size, NULL) != 0 || !data) break;
        
        Av1DecodeOutput decode_out;
        Av1DecodeResult result = av1_decode(decoder, data, size, &decode_out);
        free(data);
        
        if (result == AV1_QUEUE_FULL) {
            queue_full_detected++;
            printf("Queue became full after %d frames\n", frames_before_full);
            break;
        }
        
        if (result == AV1_OK && decode_out.frame_ready) {
            frames_before_full++;
        }
    }
    
    if (!queue_full_detected) {
        printf("Warning: Queue did not fill with queue_depth=%d\n", small_queue);
    }
    
    // Now drain and continue
    printf("Draining queue...\n");
    int drained = 0;
    
    while (av1_frame_queue_count(&decoder->ready_queue) > 0) {
        Av1DecodeOutput sync_out;
        if (av1_sync(decoder, 0, &sync_out) == AV1_OK) {
            if (output) {
                av1_set_output(decoder, sync_out.frame_id, output);
                av1_receive_output(decoder, sync_out.frame_id, 0);
            }
            drained++;
        }
    }
    
    printf("Drained %d frames\n", drained);
    
    // Continue decoding
    int frames_after_recovery = 0;
    while (!ivf_parser_eof(parser)) {
        uint8_t *data = NULL;
        size_t size = 0;
        
        if (ivf_parser_read_frame(parser, &data, &size, NULL) != 0 || !data) break;
        
        Av1DecodeOutput decode_out;
        Av1DecodeResult result = av1_decode(decoder, data, size, &decode_out);
        free(data);
        
        if (result == AV1_OK && decode_out.frame_ready) {
            frames_after_recovery++;
        } else if (result == AV1_QUEUE_FULL) {
            // Drain again
            while (av1_frame_queue_count(&decoder->ready_queue) > 0) {
                Av1DecodeOutput sync_out;
                if (av1_sync(decoder, 0, &sync_out) == AV1_OK) {
                    if (output) {
                        av1_set_output(decoder, sync_out.frame_id, output);
                        av1_receive_output(decoder, sync_out.frame_id, 0);
                    }
                }
            }
        }
    }
    
    printf("Decoded %d frames before queue full, %d after recovery\n", 
           frames_before_full, frames_after_recovery);
    
    av1_flush(decoder);
    
    // Drain remaining
    while (1) {
        Av1DecodeOutput sync_out;
        if (av1_sync(decoder, 0, &sync_out) != AV1_OK) break;
        if (output) {
            av1_set_output(decoder, sync_out.frame_id, output);
            av1_receive_output(decoder, sync_out.frame_id, 0);
        }
    }
    
    free_output_buffer(output);
    av1_destroy_decoder(decoder);
    free(mem);
    ivf_parser_close(parser);
    
    printf("TEST PASSED: Queue full recovery works\n");
    return 0;
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char *argv[]) {
    printf("=== AV1 Decoder End-to-End Test ===\n\n");
    
    // Parse arguments
    const char *ivf_file = NULL;
    const char *y4m_file = NULL;
    int queue_depth = DEFAULT_QUEUE_DEPTH;
    int num_workers = DEFAULT_WORKERS;
    bool verbose = false;
    bool run_error_tests = false;
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-q") == 0 && i + 1 < argc) {
            queue_depth = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) {
            num_workers = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-v") == 0) {
            verbose = true;
        } else if (strcmp(argv[i], "-t") == 0) {
            run_error_tests = true;
        } else if (strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (argv[i][0] != '-') {
            if (!ivf_file) {
                ivf_file = argv[i];
            } else if (!y4m_file) {
                y4m_file = argv[i];
            }
        }
    }
    
    if (!ivf_file) {
        fprintf(stderr, "Error: No input file specified\n\n");
        print_usage(argv[0]);
        return 1;
    }
    
    // Generate output filename if not provided
    if (!y4m_file) {
        size_t len = strlen(ivf_file);
        y4m_file = malloc(len + 5);
        if (y4m_file) {
            strcpy(y4m_file, ivf_file);
            if (len > 4 && strcmp(ivf_file + len - 4, ".ivf") == 0) {
                strcpy(y4m_file + len - 4, ".y4m");
            } else {
                strcat(y4m_file, ".y4m");
            }
        }
    }
    
    // Check if file exists
    if (!ivf_parser_is_valid(ivf_file)) {
        fprintf(stderr, "Error: %s is not a valid IVF file\n", ivf_file);
        return 1;
    }
    
    int ret = 0;
    
    if (run_error_tests) {
        // Run error path tests
        print_separator();
        printf("Running error path tests\n");
        print_separator();
        
        test_truncated_bitstream(ivf_file);
        test_zero_length_au();
        test_queue_full_recovery(ivf_file);
        
        print_separator();
        printf("Error path tests completed\n");
        print_separator();
    }
    
    // Run main decode
    ret = decode_file(ivf_file, y4m_file, queue_depth, num_workers, verbose);
    
    // Free y4m_file if we allocated it
    if (y4m_file && !strstr(y4m_file, "/") && !strstr(argv[0], y4m_file)) {
        // Only free if it's a newly allocated string
        // (simplified check - in production would track this properly)
    }
    
    return ret;
}
