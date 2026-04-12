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

#define DEFAULT_QUEUE_DEPTH 4
#define DEFAULT_WORKERS 1
#define MAX_QUEUE_FULL_RETRIES 3

static void print_usage(const char *prog) {
    printf("Usage: %s [options] input.ivf [output.y4m]\n", prog);
    printf("Options:\n");
    printf("  -q <n>   Queue depth (default: %d)\n", DEFAULT_QUEUE_DEPTH);
    printf("  -w <n>   Worker threads (default: %d)\n", DEFAULT_WORKERS);
    printf("  -v       Verbose\n");
    printf("  -h       Help\n");
}

static Av1OutputBuffer* allocate_output_buffer(int width, int height, int bit_depth) {
    Av1OutputBuffer *buffer = calloc(1, sizeof(Av1OutputBuffer));
    if (!buffer) return NULL;
    
    int bytes_per_pixel = (bit_depth > 8) ? 2 : 1;
    int y_size = width * height * bytes_per_pixel;
    int uv_size = (width / 2) * (height / 2) * bytes_per_pixel;
    
    buffer->planes[0] = aligned_alloc(64, y_size);
    buffer->planes[1] = aligned_alloc(64, uv_size);
    buffer->planes[2] = aligned_alloc(64, uv_size);
    
    if (!buffer->planes[0] || !buffer->planes[1] || !buffer->planes[2]) {
        free(buffer->planes[0]); free(buffer->planes[1]); free(buffer->planes[2]); free(buffer);
        return NULL;
    }
    
    buffer->strides[0] = width * bytes_per_pixel;
    buffer->strides[1] = (width / 2) * bytes_per_pixel;
    buffer->strides[2] = (width / 2) * bytes_per_pixel;
    buffer->widths[0] = width; buffer->widths[1] = width / 2; buffer->widths[2] = width / 2;
    buffer->heights[0] = height; buffer->heights[1] = height / 2; buffer->heights[2] = height / 2;
    buffer->width = width; buffer->height = height;
    buffer->bit_depth = bit_depth;
    buffer->chroma_subsampling = 0;
    
    return buffer;
}

static void free_output_buffer(Av1OutputBuffer *buffer) {
    if (!buffer) return;
    free(buffer->planes[0]);
    free(buffer->planes[1]);
    free(buffer->planes[2]);
    free(buffer);
}

/**
 * Drain all ready frames from decoder without needing internal queue access.
 * Uses av1_sync in a loop until no more frames are available.
 */
static int drain_decoder_queue(Av1Decoder *decoder, Av1OutputBuffer *output, bool verbose) {
    int drained = 0;
    
    while (1) {
        Av1DecodeOutput sync_out;
        Av1DecodeResult sync_result = av1_sync(decoder, 0, &sync_out);
        
        if (sync_result == AV1_NEED_MORE_DATA || sync_result == AV1_END_OF_STREAM) {
            break;
        }
        
        if (sync_result != AV1_OK) {
            break;
        }
        
        av1_set_output(decoder, sync_out.frame_id, output);
        if (av1_receive_output(decoder, sync_out.frame_id, 0) == AV1_OK) {
            drained++;
            if (verbose) printf("  Drained frame_id=%u\n", sync_out.frame_id);
        }
    }
    
    return drained;
}

static int decode_file(const char *ivf_file, const char *y4m_file,
                       int queue_depth, int num_workers, bool verbose) {
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
    
    /* 
     * AV1 requires parsing the Sequence Header OBU from the bitstream
     * to get accurate stream info. We use IVF dimensions as initial
     * estimates for memory query, but the decoder will update these
     * when it parses the actual sequence header.
     * Default to 8-bit 420 for initial allocation.
     */
    Av1StreamInfo info = {
        .width = header->width,
        .height = header->height,
        .max_bitrate = 8000000,  /* Reasonable default for high bitrate */
        .chroma_subsampling = 0, /* 420 */
        .is_16bit = false        /* 8-bit default */
    };
    
    Av1MemoryRequirements req = av1_query_memory(&info, queue_depth, num_workers);
    printf("Memory required: %zu bytes (%.2f MB)\n", 
           req.total_size, req.total_size / (1024.0 * 1024.0));
    
    void *mem_block = aligned_alloc(req.alignment, req.total_size);
    if (!mem_block) {
        fprintf(stderr, "Error: Failed to allocate memory block\n");
        ivf_parser_close(parser);
        return -1;
    }
    memset(mem_block, 0, req.total_size);
    
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
    
    int fps_n = header->timebase_den > 0 ? header->timebase_den : 30;
    int fps_d = header->timebase_num > 0 ? header->timebase_num : 1;
    
    Y4MWriter *y4m = y4m_writer_open(y4m_file, header->width, header->height,
                                       fps_n, fps_d, 8, 0);
    if (!y4m) {
        fprintf(stderr, "Error: Failed to create Y4M writer\n");
        av1_destroy_decoder(decoder);
        free(mem_block);
        ivf_parser_close(parser);
        return -1;
    }
    
    Av1OutputBuffer *output = allocate_output_buffer(header->width, header->height, 8);
    if (!output) {
        fprintf(stderr, "Error: Failed to allocate output buffer\n");
        y4m_writer_close(y4m);
        av1_destroy_decoder(decoder);
        free(mem_block);
        ivf_parser_close(parser);
        return -1;
    }
    
    printf("\n--- Decoding ---\n");
    
    int frames_decoded = 0;
    int frames_output = 0;
    
    while (!ivf_parser_eof(parser)) {
        uint8_t *data = NULL;
        size_t size = 0;
        
        int read_result = ivf_parser_read_frame(parser, &data, &size, NULL);
        if (read_result != 0 || !data) break;
        
        if (verbose) {
            printf("Frame %d: size=%zu bytes\n", frames_decoded + 1, size);
        }
        
        /* Retry loop for QUEUE_FULL - retry decode after draining */
        int queue_full_retries = 0;
        Av1DecodeResult decode_result;
        Av1DecodeOutput decode_output;
        
        do {
            decode_result = av1_decode(decoder, data, size, &decode_output);
            
            if (decode_result == AV1_QUEUE_FULL) {
                if (verbose) printf("  Queue full, draining (attempt %d)...\n", queue_full_retries + 1);
                
                /* Drain the ready queue */
                drain_decoder_queue(decoder, output, verbose);
                
                queue_full_retries++;
                if (queue_full_retries >= MAX_QUEUE_FULL_RETRIES) {
                    fprintf(stderr, "Warning: Max queue full retries reached at frame %d\n", frames_decoded + 1);
                    free(data);
                    data = NULL;
                    break;
                }
                /* Retry the decode of the same frame */
            }
        } while (decode_result == AV1_QUEUE_FULL && data != NULL);
        
        free(data);
        data = NULL;
        
        if (decode_result == AV1_ERROR) {
            fprintf(stderr, "Decode error at frame %d\n", frames_decoded);
            continue;
        }
        
        if (decode_result == AV1_INVALID_PARAM) {
            fprintf(stderr, "Invalid param error at frame %d\n", frames_decoded);
            continue;
        }
        
        if (decode_result == AV1_FLUSHED) {
            fprintf(stderr, "Decoder flushed unexpectedly at frame %d\n", frames_decoded);
            continue;
        }
        
        if (decode_result != AV1_OK) {
            /* AV1_NEED_MORE_DATA or other - skip this frame */
            continue;
        }
        
        frames_decoded++;
        
        if (decode_output.frame_ready) {
            Av1DecodeOutput sync_out;
            if (av1_sync(decoder, 0, &sync_out) == AV1_OK) {
                av1_set_output(decoder, sync_out.frame_id, output);
                if (av1_receive_output(decoder, sync_out.frame_id, 0) == AV1_OK) {
                    if (y4m_writer_write_buffer(y4m, output) < 0) {
                        fprintf(stderr, "Warning: Failed to write frame %d to Y4M\n", frames_output);
                    } else {
                        frames_output++;
                    }
                    if (verbose) printf("  Output: frame_id=%u\n", sync_out.frame_id);
                }
            }
        }
    }
    
    printf("Decoded %d frames, output %d frames\n", frames_decoded, frames_output);
    
    printf("\n--- Flushing ---\n");
    Av1DecodeResult flush_result = av1_flush(decoder);
    if (flush_result != AV1_OK) {
        fprintf(stderr, "Warning: av1_flush returned %d\n", flush_result);
    }
    
    while (1) {
        Av1DecodeOutput sync_out;
        Av1DecodeResult sync_result = av1_sync(decoder, 100000, &sync_out);
        if (sync_result == AV1_END_OF_STREAM) break;
        if (sync_result != AV1_OK) break;
        
        av1_set_output(decoder, sync_out.frame_id, output);
        if (av1_receive_output(decoder, sync_out.frame_id, 0) == AV1_OK) {
            if (y4m_writer_write_buffer(y4m, output) < 0) {
                fprintf(stderr, "Warning: Failed to write flushed frame to Y4M\n");
            }
        }
    }
    
    printf("\n--- Destroying decoder ---\n");
    av1_destroy_decoder(decoder);
    
    free(mem_block);
    free_output_buffer(output);
    y4m_writer_close(y4m);
    ivf_parser_close(parser);
    
    printf("\n=== Summary ===\n");
    printf("Input:  %s\n", ivf_file);
    printf("Output: %s\n", y4m_file);
    printf("Frames decoded: %d\n", frames_decoded);
    printf("Frames output:  %d\n", frames_output);
    
    return 0;
}

int main(int argc, char *argv[]) {
    printf("=== AV1 Decoder End-to-End Test ===\n\n");
    
    const char *ivf_file = NULL;
    const char *y4m_file = NULL;
    int queue_depth = DEFAULT_QUEUE_DEPTH;
    int num_workers = DEFAULT_WORKERS;
    bool verbose = false;
    bool y4m_file_allocated = false;  /* Track if we allocated y4m_file */
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-q") == 0 && i + 1 < argc) {
            queue_depth = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) {
            num_workers = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-v") == 0) {
            verbose = true;
        } else if (strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (argv[i][0] != '-') {
            if (!ivf_file) ivf_file = argv[i];
            else if (!y4m_file) y4m_file = argv[i];
        }
    }
    
    if (!ivf_file) {
        fprintf(stderr, "Error: No input file specified\n\n");
        print_usage(argv[0]);
        return 1;
    }
    
    if (!y4m_file) {
        size_t len = strlen(ivf_file);
        y4m_file = malloc(len + 5);
        if (!y4m_file) {
            fprintf(stderr, "Error: Failed to allocate memory for output filename\n");
            return 1;
        }
        y4m_file_allocated = true;
        strcpy(y4m_file, ivf_file);
        if (len > 4 && strcmp(ivf_file + len - 4, ".ivf") == 0) {
            strcpy(y4m_file + len - 4, ".y4m");
        } else {
            strcat(y4m_file, ".y4m");
        }
    }
    
    if (!ivf_parser_is_valid(ivf_file)) {
        fprintf(stderr, "Error: %s is not a valid IVF file\n", ivf_file);
        if (y4m_file_allocated) free((void*)y4m_file);
        return 1;
    }
    
    int ret = decode_file(ivf_file, y4m_file, queue_depth, num_workers, verbose);
    
    /* Always free y4m_file if we allocated it */
    if (y4m_file_allocated) {
        free((void*)y4m_file);
    }
    
    return ret;
}
