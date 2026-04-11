

I'll create the unified source tree with merged files. Let me start with the merged header files and implementation, then the Makefile.

### av1_decoder_api.h

```c
#ifndef AV1_DECODER_API_H
#define AV1_DECODER_API_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Forward declarations
struct Av1Decoder;
typedef struct Av1Decoder Av1Decoder;

// Include stream info from memory override
#include "av1_mem_override.h"

// Include copy thread for copy job handling
#include "av1_copy_thread.h"

// ============================================================================
// Memory Requirements Structure
// ============================================================================

typedef struct Av1MemoryBreakdown {
    size_t frame_buffers;      // DPB frame buffers (with border)
    size_t dpb_count;          // Number of DPB buffers
    size_t worker_scratch;     // Per-worker scratch memory
    size_t worker_count;       // Number of workers
    size_t entropy_contexts;   // Entropy context storage
    size_t decoder_context;    // Main decoder context
    size_t tile_data;          // Tile data storage
    size_t mode_info_grid;     // Mode info grid
    size_t other;              // Other allocations
} Av1MemoryBreakdown;

typedef struct Av1MemoryRequirements {
    size_t total_size;         // Total required memory
    size_t alignment;          // Required alignment (64 bytes)
    Av1MemoryBreakdown breakdown;
} Av1MemoryRequirements;

// ============================================================================
// Decoder Configuration
// ============================================================================

typedef struct Av1ThreadConfig {
    int priority;              // Thread priority (0 = default, higher = higher)
    int cpu_affinity;          // CPU affinity mask (-1 = no affinity)
    int core_id;               // Specific core to bind to (-1 = any)
} Av1ThreadConfig;

typedef struct Av1DecoderConfig {
    // Memory
    void        *memory_base;      // Caller-provided memory block
    size_t       memory_size;      // Size of memory block
    
    // Queue configuration
    int          queue_depth;      // Number of frames to buffer for reordering
    
    // Threading
    int          num_worker_threads;
    Av1ThreadConfig worker_config;     // Worker thread settings
    Av1ThreadConfig copy_thread_config; // Copy thread settings
    
    // GPU configuration
    bool         use_gpu;          // Enable GPU offload
    int          gpu_device;       // GPU device ID (0 = default)
    Av1ThreadConfig gpu_thread_config; // GPU thread settings
    
    // Decoder options
    bool         enable_threading; // Enable multi-threaded decoding
    bool         enable_frame_parallel; // Enable frame-level parallelism
    int          max_tile_cols;    // Max tile columns (0 = auto)
    int          max_tile_rows;    // Max tile rows (0 = auto)
} Av1DecoderConfig;

// ============================================================================
// Decoder State
// ============================================================================

typedef enum {
    AV1_DECODER_STATE_UNINITIALIZED = 0,
    AV1_DECODER_STATE_CREATED,
    AV1_DECODER_STATE_READY,
    AV1_DECODER_STATE_DECODING,
    AV1_DECODER_STATE_FLUSHING,
    AV1_DECODER_STATE_ERROR
} Av1DecoderState;

// ============================================================================
// Decode Result
// ============================================================================

typedef enum {
    AV1_OK = 0,
    AV1_ERROR = -1,
    AV1_QUEUE_FULL = -2,
    AV1_INVALID_PARAM = -3,
    AV1_NEED_MORE_DATA = -4,
    AV1_END_OF_STREAM = -5,
    AV1_FLUSHED = -6           // Decoder is flushed, no more frames
} Av1DecodeResult;

// ============================================================================
// Decode Output
// ============================================================================

typedef struct Av1DecodeOutput {
    int frame_ready;           // 1 if a frame is ready for output
    uint32_t frame_id;         // Frame identifier (monotonic)
    int show_existing_frame;   // 1 if this is a show_existing_frame
    int dpb_slot;              // DPB slot where frame is stored
} Av1DecodeOutput;

// ============================================================================
// Output Buffer
// ============================================================================

typedef struct Av1OutputBuffer {
    uint8_t       *planes[3];      // Y, U, V plane pointers
    int            strides[3];     // Stride for each plane
    int            widths[3];      // Width in pixels for each plane
    int            heights[3];     // Height in pixels for each plane
    int            width;          // Frame width
    int            height;         // Frame height
    int            bit_depth;      // Bit depth (8, 10, 12)
    int            chroma_subsampling; // 0=420, 1=422, 2=444
} Av1OutputBuffer;

// ============================================================================
// Pending Output Entry
// ============================================================================

typedef struct Av1PendingOutput {
    uint32_t       frame_id;      // Frame identifier
    int            dpb_slot;      // DPB slot where frame is stored
    Av1CopyJob    *copy_job;      // Copy job (NULL if not yet copied)
    int            ref_count;     // Reference count for DPB
    int            valid;         // 1 if entry is valid
} Av1PendingOutput;

// ============================================================================
// Decoder Handle (Opaque)
// ============================================================================

// The Av1Decoder struct is defined in the implementation file
// Users only get a pointer to it

// ============================================================================
// API Functions
// ============================================================================

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Query memory requirements for a given stream configuration.
 * 
 * @param info           Stream information (max dimensions, bit depth, etc.)
 * @param queue_depth    Number of frames to buffer for reordering
 * @param num_workers    Number of decoder worker threads
 * @return Av1MemoryRequirements with total size and breakdown
 */
Av1MemoryRequirements av1_query_memory(const Av1StreamInfo *info, 
                                        int queue_depth, 
                                        int num_workers);

/**
 * Create and initialize an AV1 decoder instance.
 * 
 * @param config Decoder configuration
 * @return Decoder handle on success, NULL on failure
 */
Av1Decoder *av1_create_decoder(const Av1DecoderConfig *config);

/**
 * Flush the decoder and signal end of stream.
 * 
 * After calling this, the decoder enters FLUSHING state and will reject
 * any subsequent av1_decode() calls. The app should drain remaining frames
 * via av1_sync() until it returns AV1_END_OF_STREAM.
 * 
 * @param decoder Decoder handle
 * @return AV1_OK on success, AV1_INVALID_PARAM if decoder is NULL
 */
Av1DecodeResult av1_flush(Av1Decoder *decoder);

/**
 * Destroy a decoder instance and release all resources.
 * 
 * This function performs a complete cleanup:
 * - If not already flushed, implicitly flushes and discards pending frames
 * - Waits for any in-progress copy operations to complete
 * - Signals and joins all worker threads (copy, worker, GPU)
 * - Destroys the AOM codec context
 * - Destroys synchronization primitives
 * - Clears the memory pool flag
 * 
 * @param decoder Decoder handle to destroy
 * @return 0 on success, -1 on error (NULL decoder)
 */
int av1_destroy_decoder(Av1Decoder *decoder);

/**
 * Get the current state of the decoder.
 * 
 * @param decoder Decoder handle
 * @return Current decoder state
 */
Av1DecoderState av1_get_decoder_state(const Av1Decoder *decoder);

/**
 * Get decoder configuration (for inspection).
 * 
 * @param decoder Decoder handle
 * @return Pointer to internal config (do not modify), or NULL on error
 */
const Av1DecoderConfig *av1_get_decoder_config(const Av1Decoder *decoder);

/**
 * Get memory statistics from the allocator.
 * 
 * @param decoder Decoder handle
 * @return Memory statistics (see av1_mem_override.h)
 */
Av1MemStats av1_get_mem_stats(const Av1Decoder *decoder);

/**
 * Reset memory statistics.
 * 
 * @param decoder Decoder handle
 */
void av1_reset_mem_stats(Av1Decoder *decoder);

/**
 * Decode an access unit (AU) of AV1 data.
 * 
 * This function decodes a single frame (or multiple OBUs forming an AU)
 * synchronously. The decoded frame is placed in the ready queue if it
 * should be displayed.
 * 
 * @param decoder    Decoder handle
 * @param data       Pointer to AU data (OBUs)
 * @param data_size  Size of data in bytes
 * @param out_result Output structure filled with frame info (can be NULL)
 * @return AV1_OK on success, AV1_ERROR on error, AV1_QUEUE_FULL if ready
 *         queue is full, AV1_INVALID_PARAM if decoder state is invalid,
 *         AV1_FLUSHED if decoder has been flushed
 */
Av1DecodeResult av1_decode(Av1Decoder *decoder, 
                           const uint8_t *data, 
                           size_t data_size,
                           Av1DecodeOutput *out_result);

/**
 * Signal end of stream to the decoder (legacy, use av1_flush instead).
 * 
 * @param decoder Decoder handle
 * @return AV1_OK on success
 */
Av1DecodeResult av1_decode_end(Av1Decoder *decoder);

/**
 * Synchronize - get next decoded frame from ready queue.
 * 
 * This function pops a frame from the ready queue and moves it to the
 * pending output table. The frame is then ready for av1_set_output().
 * 
 * @param decoder    Decoder handle
 * @param timeout_us Timeout in microseconds (0 = infinite wait)
 * @param out_result Output structure filled with frame info (can be NULL)
 * @return AV1_OK on success (frame ready), AV1_NEED_MORE_DATA if timeout,
 *         AV1_END_OF_STREAM if flushing and no more frames
 */
Av1DecodeResult av1_sync(Av1Decoder *decoder, 
                         uint32_t timeout_us,
                         Av1DecodeOutput *out_result);

/**
 * Set output buffer for a frame.
 * 
 * This function looks up the frame in the pending output table, gets the
 * DPB slot, builds a copy job, and enqueues it to the copy thread.
 * 
 * @param decoder       Decoder handle
 * @param frame_id      Frame ID to output
 * @param output_buffer Output buffer structure with destination pointers
 * @return AV1_OK on success, AV1_ERROR if frame not found
 */
Av1DecodeResult av1_set_output(Av1Decoder *decoder, 
                               uint32_t frame_id,
                               const Av1OutputBuffer *output_buffer);

/**
 * Wait for output copy to complete and release resources.
 * 
 * This function waits for the copy job to complete, then releases the
 * DPB reference. After this call, the frame data is available in the
 * output buffer and the DPB slot is freed.
 * 
 * @param decoder    Decoder handle
 * @param frame_id   Frame ID to receive
 * @param timeout_us Timeout in microseconds (0 = infinite wait)
 * @return AV1_OK on success, AV1_ERROR on timeout or error
 */
Av1DecodeResult av1_receive_output(Av1Decoder *decoder, 
                                    uint32_t frame_id,
                                    uint32_t timeout_us);

/**
 * Get the next decoded frame from the ready queue (legacy).
 * 
 * @param decoder    Decoder handle
 * @param out_entry  Output structure for frame entry
 * @param timeout_us Timeout in microseconds (0 = non-blocking)
 * @return AV1_OK on success, AV1_ERROR if no frame available
 */
Av1DecodeResult av1_get_decoded_frame(Av1Decoder *decoder, 
                                       Av1FrameEntry *out_entry,
                                       uint32_t timeout_us);

/**
 * Release a frame back to the decoder (after output).
 * 
 * @param decoder  Decoder handle
 * @param frame_id Frame ID to release
 * @return AV1_OK on success
 */
Av1DecodeResult av1_release_frame(Av1Decoder *decoder, uint32_t frame_id);

#ifdef __cplusplus
}
#endif

#endif // AV1_DECODER_API_H
```

### av1_decoder_api.c

```c
#define AV1_DECODER_API_C
#include "av1_decoder_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdalign.h>
#include <sched.h>
#include <errno.h>

// Include the memory override and job queue
#include "av1_mem_override.h"
#include "av1_job_queue.h"

// ============================================================================
// AOM Integration
// ============================================================================

// Include AOM headers for decoder integration
#include "aom/aom_decoder.h"
#include "aom/aomdx.h"

// ============================================================================
// Constants
// ============================================================================

#define DEFAULT_ALIGNMENT      64
#define MC_BORDER              128           // Motion compensation border
#define BASE_DPB_COUNT         8
#define ENTROPY_CONTEXT_PER_FRAME (75 * 1024)  // ~75KB per frame
#define WORKER_SCRATCH_SIZE    (2 * 1024 * 1024)  // 2MB per worker
#define DECODER_CONTEXT_SIZE   (8 * 1024 * 1024)  // 8MB
#define TILE_DATA_SIZE         (4 * 1024 * 1024)  // 4MB
#define MODE_INFO_GRID_SIZE    (2 * 1024 * 1024) // 2MB
#define HEADROOM_FACTOR        1.10  // 10% headroom
#define MAX_PENDING_OUTPUT     16    // Max pending output entries

// ============================================================================
// Internal Structures
// ============================================================================

typedef struct Av1WorkerThread {
    pthread_t thread;
    int thread_id;
    Av1ThreadConfig config;
    bool running;
    void *user_data;  // For future use
} Av1WorkerThread;

typedef struct Av1GPUThread {
    pthread_t thread;
    Av1ThreadConfig config;
    bool running;
    int gpu_device;
    void *film_grain_buffer;  // For future GPU film grain
} Av1GPUThread;

// DPB slot structure (simulated for this implementation)
typedef struct Av1DPBSlot {
    uint8_t *planes[3];
    int strides[3];
    int widths[3];
    int heights[3];
    int width;
    int height;
    int bit_depth;
    int in_use;
    int ref_count;
} Av1DPBSlot;

struct Av1Decoder {
    // Configuration
    Av1DecoderConfig config;
    Av1DecoderState state;
    
    // Memory allocator state
    void *memory_base;
    size_t memory_size;
    bool mem_allocator_initialized;
    
    // AOM decoder context
    aom_codec_ctx_t aom_decoder;
    bool aom_decoder_initialized;
    
    // Frame queues
    Av1FrameQueue ready_queue;         // Decoded frames ready for output
    Av1FrameQueue output_queue;        // Frames pending output pickup
    
    // Queue storage
    Av1FrameEntry *queue_storage;
    int queue_storage_size;
    
    // Frame ID counter (monotonic)
    uint32_t next_frame_id;
    
    // Pending output table
    Av1PendingOutput pending_output[MAX_PENDING_OUTPUT];
    int pending_output_count;
    
    // DPB (Decoded Picture Buffer)
    Av1DPBSlot dpb[16];  // 16 slots
    int dpb_count;
    
    // Copy thread
    Av1CopyThread *copy_thread;
    
    // Worker threads
    Av1WorkerThread *workers;
    int num_workers;
    
    // GPU thread (optional)
    Av1GPUThread *gpu_thread;
    bool has_gpu_thread;
    
    // Statistics
    uint64_t frames_decoded;
    uint64_t frames_output;
    uint64_t decode_errors;
    
    // Thread safety
    pthread_mutex_t decoder_mutex;
    pthread_cond_t decoder_cond;
    
    // Destroy tracking
    int destroyed;  // 1 if destroy has been called
};

// ============================================================================
// Internal Functions
// ============================================================================

static size_t calculate_frame_buffer_size(int width, int height, int bit_depth, 
                                           int chroma_subsampling) {
    // Add border for motion compensation
    int padded_width = width + 2 * MC_BORDER;
    int padded_height = height + 2 * MC_BORDER;
    
    // Bytes per pixel
    int bytes_per_pixel = (bit_depth > 8) ? 2 : 1;
    
    // Chroma factor
    double chroma_factor;
    switch (chroma_subsampling) {
        case 2: chroma_factor = 3.0; break;   // 444
        case 1: chroma_factor = 2.0; break;   // 422
        default: chroma_factor = 1.5; break;  // 420
    }
    
    // Total size
    size_t size = (size_t)padded_width * padded_height * bytes_per_pixel * (size_t)chroma_factor;
    
    // Add some overhead for alignment and additional planes
    size = (size + 4095) & ~4095;  // 4KB alignment
    
    return size;
}

static int set_thread_priority(pthread_t thread, const Av1ThreadConfig *config) {
    if (!config) {
        return 0;
    }
    
    // Set thread priority if specified
    if (config->priority != 0) {
        struct sched_param param;
        int policy;
        
        if (pthread_getschedparam(thread, &policy, &param) == 0) {
            param.sched_priority = config->priority;
            // Clamp priority to valid range
            int min_priority = sched_get_priority_min(policy);
            int max_priority = sched_get_priority_max(policy);
            if (param.sched_priority < min_priority) {
                param.sched_priority = min_priority;
            }
            if (param.sched_priority > max_priority) {
                param.sched_priority = max_priority;
            }
            pthread_setschedparam(thread, policy, &param);
        }
    }
    
    // Set CPU affinity if specified
    if (config->cpu_affinity >= 0) {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(config->cpu_affinity, &cpuset);
        pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset);
    }
    
    return 0;
}

// Find free DPB slot
static int find_free_dpb_slot(Av1Decoder *decoder) {
    for (int i = 0; i < decoder->dpb_count; i++) {
        if (!decoder->dpb[i].in_use) {
            return i;
        }
    }
    return -1;
}

// Allocate DPB slot for a frame
static int allocate_dpb_slot(Av1Decoder *decoder, int width, int height, int bit_depth) {
    int slot = find_free_dpb_slot(decoder);
    if (slot < 0) {
        return -1;
    }
    
    Av1DPBSlot *dpb_slot = &decoder->dpb[slot];
    
    // Calculate sizes
    int bytes_per_pixel = (bit_depth > 8) ? 2 : 1;
    int y_size = width * height * bytes_per_pixel;
    int uv_size = (width / 2) * (height / 2) * bytes_per_pixel;
    
    // Allocate planes
    dpb_slot->planes[0] = (uint8_t *)av1_mem_memalign(64, y_size);
    dpb_slot->planes[1] = (uint8_t *)av1_mem_memalign(64, uv_size);
    dpb_slot->planes[2] = (uint8_t *)av1_mem_memalign(64, uv_size);
    
    if (!dpb_slot->planes[0] || !dpb_slot->planes[1] || !dpb_slot->planes[2]) {
        if (dpb_slot->planes[0]) av1_mem_free(dpb_slot->planes[0]);
        if (dpb_slot->planes[1]) av1_mem_free(dpb_slot->planes[1]);
        if (dpb_slot->planes[2]) av1_mem_free(dpb_slot->planes[2]);
        return -1;
    }
    
    dpb_slot->strides[0] = width * bytes_per_pixel;
    dpb_slot->strides[1] = (width / 2) * bytes_per_pixel;
    dpb_slot->strides[2] = (width / 2) * bytes_per_pixel;
    
    dpb_slot->width = width;
    dpb_slot->height = height;
    dpb_slot->bit_depth = bit_depth;
    dpb_slot->in_use = 1;
    dpb_slot->ref_count = 1;
    
    return slot;
}

// Release DPB slot
static void release_dpb_slot(Av1Decoder *decoder, int slot) {
    if (slot < 0 || slot >= decoder->dpb_count) {
        return;
    }
    
    Av1DPBSlot *dpb_slot = &decoder->dpb[slot];
    dpb_slot->ref_count--;
    
    if (dpb_slot->ref_count <= 0) {
        if (dpb_slot->planes[0]) av1_mem_free(dpb_slot->planes[0]);
        if (dpb_slot->planes[1]) av1_mem_free(dpb_slot->planes[1]);
        if (dpb_slot->planes[2]) av1_mem_free(dpb_slot->planes[2]);
        
        dpb_slot->planes[0] = NULL;
        dpb_slot->planes[1] = NULL;
        dpb_slot->planes[2] = NULL;
        dpb_slot->in_use = 0;
    }
}

// Find pending output entry by frame_id
static Av1PendingOutput* find_pending_output(Av1Decoder *decoder, uint32_t frame_id) {
    for (int i = 0; i < MAX_PENDING_OUTPUT; i++) {
        if (decoder->pending_output[i].valid && 
            decoder->pending_output[i].frame_id == frame_id) {
            return &decoder->pending_output[i];
        }
    }
    return NULL;
}

// Add to pending output table
static int add_pending_output(Av1Decoder *decoder, uint32_t frame_id, int dpb_slot) {
    // Find free slot
    for (int i = 0; i < MAX_PENDING_OUTPUT; i++) {
        if (!decoder->pending_output[i].valid) {
            decoder->pending_output[i].frame_id = frame_id;
            decoder->pending_output[i].dpb_slot = dpb_slot;
            decoder->pending_output[i].copy_job = NULL;
            decoder->pending_output[i].ref_count = 1;
            decoder->pending_output[i].valid = 1;
            decoder->pending_output_count++;
            return 0;
        }
    }
    return -1;
}

// Remove from pending output table
static void remove_pending_output(Av1Decoder *decoder, uint32_t frame_id) {
    for (int i = 0; i < MAX_PENDING_OUTPUT; i++) {
        if (decoder->pending_output[i].valid && 
            decoder->pending_output[i].frame_id == frame_id) {
            decoder->pending_output[i].valid = 0;
            decoder->pending_output[i].copy_job = NULL;
            decoder->pending_output_count--;
            return;
        }
    }
}

// Clear all pending output entries (for destroy)
static void clear_all_pending_output(Av1Decoder *decoder) {
    for (int i = 0; i < MAX_PENDING_OUTPUT; i++) {
        if (decoder->pending_output[i].valid) {
            // Release DPB slot
            if (decoder->pending_output[i].dpb_slot >= 0) {
                release_dpb_slot(decoder, decoder->pending_output[i].dpb_slot);
            }
            // Free copy job if exists
            if (decoder->pending_output[i].copy_job) {
                free(decoder->pending_output[i].copy_job);
            }
            decoder->pending_output[i].valid = 0;
            decoder->pending_output[i].copy_job = NULL;
        }
    }
    decoder->pending_output_count = 0;
}

// Drain ready queue (discard all frames)
static void drain_ready_queue(Av1Decoder *decoder) {
    Av1FrameEntry entry;
    while (av1_frame_queue_pop(&decoder->ready_queue, &entry, 0) == 0) {
        // Release DPB slot
        if (entry.dpb_slot >= 0) {
            release_dpb_slot(decoder, entry.dpb_slot);
        }
    }
}

// ============================================================================
// Worker Thread Implementation (Stub)
// ============================================================================

static void *worker_thread_func(void *arg) {
    Av1WorkerThread *worker = (Av1WorkerThread *)arg;
    
    printf("Worker thread %d started\n", worker->thread_id);
    
    // Set thread priority/affinity
    set_thread_priority(worker->thread, &worker->config);
    
    // Stub: In a real implementation, this would process decode jobs
    worker->running = true;
    
    printf("Worker thread %d exiting\n", worker->thread_id);
    return NULL;
}

// ============================================================================
// GPU Thread Implementation (Stub)
// ============================================================================

static void *gpu_thread_func(void *arg) {
    Av1GPUThread *gpu = (Av1GPUThread *)arg;
    
    printf("GPU thread started (device %d)\n", gpu->gpu_device);
    
    set_thread_priority(gpu->thread, &gpu->config);
    
    gpu->running = true;
    
    // Stub: In real implementation, would handle GPU offload
    printf("GPU thread exiting\n");
    return NULL;
}

// ============================================================================
// API Implementation
// ============================================================================

Av1MemoryRequirements av1_query_memory(const Av1StreamInfo *info, 
                                        int queue_depth, 
                                        int num_workers) {
    Av1MemoryRequirements req = {0};
    
    if (!info || queue_depth < 0 || num_workers < 0) {
        fprintf(stderr, "av1_query_memory: invalid parameters\n");
        return req;
    }
    
    // Use provided dimensions or defaults
    int width = info->width > 0 ? info->width : 1920;
    int height = info->height > 0 ? info->height : 1080;
    int bit_depth = info->is_16bit ? 12 : (info->max_bitrate > 8 ? 10 : 8);
    int chroma = info->chroma_subsampling;
    
    // 1. Frame buffer size
    size_t frame_buffer_size = calculate_frame_buffer_size(width, height, bit_depth, chroma);
    
    // 2. DPB count: 8 base + queue_depth + 1 for current frame
    int dpb_count = BASE_DPB_COUNT + queue_depth + 1;
    req.breakdown.dpb_count = dpb_count;
    req.breakdown.frame_buffers = frame_buffer_size * dpb_count;
    
    // 3. Worker scratch memory
    int workers = num_workers > 0 ? num_workers : 4;
    req.breakdown.worker_count = workers;
    req.breakdown.worker_scratch = (size_t)workers * WORKER_SCRATCH_SIZE;
    
    // 4. Entropy contexts
    req.breakdown.entropy_contexts = (size_t)(queue_depth + 1) * ENTROPY_CONTEXT_PER_FRAME;
    
    // 5. Decoder context
    req.breakdown.decoder_context = DECODER_CONTEXT_SIZE;
    
    // 6. Tile data
    req.breakdown.tile_data = TILE_DATA_SIZE;
    
    // 7. Mode info grid
    req.breakdown.mode_info_grid = MODE_INFO_GRID_SIZE;
    
    // 8. Other (queues, thread structures, etc.)
    size_t queue_storage = (size_t)(queue_depth + 4) * sizeof(Av1FrameEntry) * 2;
    size_t thread_structs = (size_t)workers * sizeof(Av1WorkerThread) + 
                            sizeof(Av1CopyThread) +
                            sizeof(Av1GPUThread);
    size_t decoder_struct = sizeof(Av1Decoder);
    req.breakdown.other = queue_storage + thread_structs + decoder_struct;
    
    // Sum all components
    size_t total = req.breakdown.frame_buffers +
                   req.breakdown.worker_scratch +
                   req.breakdown.entropy_contexts +
                   req.breakdown.decoder_context +
                   req.breakdown.tile_data +
                   req.breakdown.mode_info_grid +
                   req.breakdown.other;
    
    // Apply headroom
    total = (size_t)((double)total * HEADROOM_FACTOR);
    
    // Apply alignment
    req.total_size = (total + DEFAULT_ALIGNMENT - 1) & ~(DEFAULT_ALIGNMENT - 1);
    req.alignment = DEFAULT_ALIGNMENT;
    
    return req;
}

Av1Decoder *av1_create_decoder(const Av1DecoderConfig *config) {
    if (!config) {
        fprintf(stderr, "av1_create_decoder: NULL config\n");
        return NULL;
    }
    
    // Validate configuration
    if (!config->memory_base || config->memory_size == 0) {
        fprintf(stderr, "av1_create_decoder: invalid memory base or size\n");
        return NULL;
    }
    
    if (config->queue_depth < 0) {
        fprintf(stderr, "av1_create_decoder: invalid queue depth\n");
        return NULL;
    }
    
    // Query memory requirements
    Av1StreamInfo info = {
        .width = config->memory_size > 0 ? 1920 : 0,
        .height = 1080,
        .max_bitrate = 10,
        .chroma_subsampling = 0,
        .is_16bit = false
    };
    
    Av1MemoryRequirements req = av1_query_memory(&info, 
                                                   config->queue_depth, 
                                                   config->num_worker_threads);
    
    if (config->memory_size < req.total_size) {
        fprintf(stderr, "av1_create_decoder: memory too small. Need %zu bytes, got %zu\n",
                req.total_size, config->memory_size);
        return NULL;
    }
    
    // Initialize memory allocator
    if (!av1_mem_init(config->memory_base, config->memory_size)) {
        fprintf(stderr, "av1_create_decoder: failed to initialize memory allocator\n");
        return NULL;
    }
    
    // Enable memory override
    av1_mem_set_override_enabled(true);
    
    // Allocate decoder structure
    Av1Decoder *decoder = (Av1Decoder *)av1_mem_memalign(DEFAULT_ALIGNMENT, sizeof(Av1Decoder));
    if (!decoder) {
        fprintf(stderr, "av1_create_decoder: failed to allocate decoder struct\n");
        av1_mem_set_override_enabled(false);
        av1_mem_shutdown();
        return NULL;
    }
    
    // Initialize decoder structure
    memset(decoder, 0, sizeof(Av1Decoder));
    decoder->config = *config;
    decoder->state = AV1_DECODER_STATE_CREATED;
    decoder->memory_base = config->memory_base;
    decoder->memory_size = config->memory_size;
    decoder->mem_allocator_initialized = true;
    decoder->next_frame_id = 0;
    decoder->pending_output_count = 0;
    decoder->dpb_count = 16;
    decoder->destroyed = 0;
    
    // Initialize DPB
    memset(decoder->dpb, 0, sizeof(decoder->dpb));
    
    // Initialize pending output table
    memset(decoder->pending_output, 0, sizeof(decoder->pending_output));
    
    // Initialize AOM decoder
    memset(&decoder->aom_decoder, 0, sizeof(decoder->aom_decoder));
    
    // Configure AOM decoder
    aom_codec_dec_cfg_t aom_config = {
        .threads = config->num_worker_threads > 0 ? config->num_worker_threads : 1,
        .width = 0,
        .height = 0,
        .alloc_buf = NULL,
        .alloc_buf_size = 0,
        .init_flags = 0
    };
    
    if (config->enable_threading) {
        aom_config.init_flags |= AOM_CODEC_USE_THREADS;
    }
    
    // Initialize AOM decoder
    aom_codec_err_t aom_err = aom_codec_init(&aom_codec_av1_dx, &aom_config, &decoder->aom_decoder);
    if (aom_err != AOM_CODEC_OK) {
        fprintf(stderr, "av1_create_decoder: failed to initialize AOM decoder: %s\n",
                aom_codec_error(&decoder->aom_decoder));
        goto cleanup_decoder;
    }
    
    decoder->aom_decoder_initialized = true;
    
    // Initialize mutex and condition variable
    if (pthread_mutex_init(&decoder->decoder_mutex, NULL) != 0) {
        fprintf(stderr, "av1_create_decoder: failed to init mutex\n");
        goto cleanup_aom;
    }
    
    if (pthread_cond_init(&decoder->decoder_cond, NULL) != 0) {
        fprintf(stderr, "av1_create_decoder: failed to init cond\n");
        pthread_mutex_destroy(&decoder->decoder_mutex);
        goto cleanup_aom;
    }
    
    // Allocate queue storage
    int queue_capacity = config->queue_depth + 4;
    decoder->queue_storage_size = queue_capacity * sizeof(Av1FrameEntry) * 2;
    decoder->queue_storage = (Av1FrameEntry *)av1_mem_memalign(DEFAULT_ALIGNMENT, 
                                                                 decoder->queue_storage_size);
    if (!decoder->queue_storage) {
        fprintf(stderr, "av1_create_decoder: failed to allocate queue storage\n");
        goto cleanup_sync;
    }
    
    // Initialize ready queue
    if (av1_frame_queue_init(&decoder->ready_queue, decoder->queue_storage, 
                              queue_capacity) != 0) {
        fprintf(stderr, "av1_create_decoder: failed to init ready queue\n");
        goto cleanup_queue_storage;
    }
    
    // Initialize output queue
    Av1FrameEntry *output_storage = decoder->queue_storage + queue_capacity;
    if (av1_frame_queue_init(&decoder->output_queue, output_storage, 
                              queue_capacity) != 0) {
        fprintf(stderr, "av1_create_decoder: failed to init output queue\n");
        goto cleanup_ready_queue;
    }
    
    // Create copy thread
    decoder->copy_thread = av1_copy_thread_create(queue_capacity);
    if (!decoder->copy_thread) {
        fprintf(stderr, "av1_create_decoder: failed to create copy thread\n");
        goto cleanup_output_queue;
    }
    
    // Allocate and create worker threads
    int num_workers = config->num_worker_threads > 0 ? config->num_worker_threads : 4;
    decoder->num_workers = num_workers;
    
    if (num_workers > 0) {
        decoder->workers = (Av1WorkerThread *)av1_mem_calloc(num_workers, sizeof(Av1WorkerThread));
        if (!decoder->workers) {
            fprintf(stderr, "av1_create_decoder: failed to allocate workers\n");
            goto cleanup_copy_thread;
        }
        
        for (int i = 0; i < num_workers; i++) {
            decoder->workers[i].thread_id = i;
            decoder->workers[i].config = config->worker_config;
            decoder->workers[i].running = false;
            
            if (pthread_create(&decoder->workers[i].thread, NULL, 
                               worker_thread_func, &decoder->workers[i]) != 0) {
                fprintf(stderr, "av1_create_decoder: failed to create worker %d\n", i);
                for (int j = 0; j < i; j++) {
                    pthread_cancel(decoder->workers[j].thread);
                    pthread_join(decoder->workers[j].thread, NULL);
                }
                av1_mem_free(decoder->workers);
                decoder->workers = NULL;
                goto cleanup_copy_thread;
            }
        }
    }
    
    // Optionally create GPU thread
    if (config->use_gpu) {
        decoder->gpu_thread = (Av1GPUThread *)av1_mem_calloc(1, sizeof(Av1GPUThread));
        if (!decoder->gpu_thread) {
            fprintf(stderr, "av1_create_decoder: failed to allocate GPU thread\n");
        } else {
            decoder->gpu_thread->config = config->gpu_thread_config;
            decoder->gpu_thread->gpu_device = config->gpu_device;
            decoder->gpu_thread->running = false;
            decoder->has_gpu_thread = true;
            
            if (pthread_create(&decoder->gpu_thread->thread, NULL, 
                               gpu_thread_func, decoder->gpu_thread) != 0) {
                fprintf(stderr, "av1_create_decoder: failed to create GPU thread\n");
                av1_mem_free(decoder->gpu_thread);
                decoder->gpu_thread = NULL;
                decoder->has_gpu_thread = false;
            }
        }
    }
    
    // Initialize statistics
    decoder->frames_decoded = 0;
    decoder->frames_output = 0;
    decoder->decode_errors = 0;
    
    // Set state to READY
    decoder->state = AV1_DECODER_STATE_READY;
    
    printf("Decoder created successfully:\n");
    printf("  Queue depth: %d\n", config->queue_depth);
    printf("  Workers: %d\n", num_workers);
    printf("  GPU: %s\n", config->use_gpu ? "enabled" : "disabled");
    
    return decoder;

cleanup_copy_thread:
    if (decoder->copy_thread) {
        av1_copy_thread_destroy(decoder->copy_thread);
    }

cleanup_output_queue:
    av1_frame_queue_destroy(&decoder->output_queue);

cleanup_ready_queue:
    av1_frame_queue_destroy(&decoder->ready_queue);

cleanup_queue_storage:
    if (decoder->queue_storage) {
        av1_mem_free(decoder->queue_storage);
    }

cleanup_sync:
    pthread_cond_destroy(&decoder->decoder_cond);
    pthread_mutex_destroy(&decoder->decoder_mutex);

cleanup_aom:
    if (decoder->aom_decoder_initialized) {
        aom_codec_destroy(&decoder->aom_decoder);
    }

cleanup_decoder:
    av1_mem_free(decoder);
    av1_mem_set_override_enabled(false);
    av1_mem_shutdown();
    
    return NULL;
}

// ============================================================================
// av1_flush Implementation
// ============================================================================

Av1DecodeResult av1_flush(Av1Decoder *decoder) {
    if (!decoder) {
        fprintf(stderr, "av1_flush: NULL decoder\n");
        return AV1_INVALID_PARAM;
    }
    
    // Check if already flushed/destroyed
    if (decoder->state == AV1_DECODER_STATE_FLUSHING ||
        decoder->state == AV1_DECODER_STATE_UNINITIALIZED ||
        decoder->destroyed) {
        // Already flushed or being destroyed
        return AV1_OK;
    }
    
    // Set state to FLUSHING
    decoder->state = AV1_DECODER_STATE_FLUSHING;
    
    // Flush AOM decoder to get remaining frames
    aom_codec_err_t aom_err = aom_codec_decode(&decoder->aom_decoder, NULL, 0, NULL);
    if (aom_err != AOM_CODEC_OK) {
        // Ignore flush errors
    }
    
    // Try to get any remaining frames from AOM
    aom_image_t *img;
    while ((img = aom_codec_get_frame(&decoder->aom_decoder, &decoder->aom_decoder.iter)) != NULL) {
        uint32_t frame_id = decoder->next_frame_id++;
        int dpb_slot = allocate_dpb_slot(decoder, img->w, img->h, img->bit_depth);
        
        if (dpb_slot >= 0) {
            Av1FrameEntry entry = {
                .frame_id = frame_id,
                .dpb_slot = dpb_slot,
                .show_frame = 1,
                .show_existing_frame = 0
            };
            av1_frame_queue_push(&decoder->ready_queue, &entry);
            decoder->frames_decoded++;
        }
    }
    
    printf("av1_flush: decoder flushed, ready queue has %d frames\n",
           av1_frame_queue_count(&decoder->ready_queue));
    
    return AV1_OK;
}

// ============================================================================
// av1_destroy_decoder Implementation
// ============================================================================

int av1_destroy_decoder(Av1Decoder *decoder) {
    if (!decoder) {
        fprintf(stderr, "av1_destroy_decoder: NULL decoder\n");
        return -1;
    }
    
    // Check for double-destroy
    if (decoder->destroyed) {
        fprintf(stderr, "av1_destroy_decoder: decoder already destroyed\n");
        return -1;
    }
    
    // Mark as destroyed to prevent further operations
    decoder->destroyed = 1;
    
    // Step 1: If not flushed, flush and drain
    if (decoder->state != AV1_DECODER_STATE_FLUSHING &&
        decoder->state != AV1_DECODER_STATE_UNINITIALIZED) {
        
        printf("av1_destroy_decoder: implicit flush before destroy\n");
        
        // Flush AOM to get any remaining frames
        aom_codec_decode(&decoder->aom_decoder, NULL, 0, NULL);
        
        // Drain ready queue (discard frames)
        drain_ready_queue(decoder);
        
        // Clear pending output table
        clear_all_pending_output(decoder);
        
        // Set state to flushing
        decoder->state = AV1_DECODER_STATE_FLUSHING;
    }
    
    // Step 2: Wait for any in-progress copy operations
    // Check pending output for any copy jobs in progress
    for (int i = 0; i < MAX_PENDING_OUTPUT; i++) {
        if (decoder->pending_output[i].valid && 
            decoder->pending_output[i].copy_job != NULL) {
            
            // Wait for copy to complete
            int status = av1_copy_thread_get_status(decoder->pending_output[i].copy_job);
            if (status == AV1_COPY_IN_PROGRESS) {
                printf("av1_destroy_decoder: waiting for in-progress copy job\n");
                av1_copy_thread_wait(decoder->copy_thread, 
                                     decoder->pending_output[i].copy_job, 
                                     5000000);  // 5 second timeout
            }
            
            // Free copy job
            free(decoder->pending_output[i].copy_job);
            decoder->pending_output[i].copy_job = NULL;
        }
    }
    
    // Step 3: Signal and join copy thread
    if (decoder->copy_thread) {
        printf("av1_destroy_decoder: stopping copy thread\n");
        av1_copy_thread_destroy(decoder->copy_thread);
        decoder->copy_thread = NULL;
    }
    
    // Step 4: Signal and join worker threads
    if (decoder->workers) {
        printf("av1_destroy_decoder: stopping worker threads\n");
        for (int i = 0; i < decoder->num_workers; i++) {
            if (decoder->workers[i].running) {
                pthread_cancel(decoder->workers[i].thread);
            }
            pthread_join(decoder->workers[i].thread, NULL);
        }
        av1_mem_free(decoder->workers);
        decoder->workers = NULL;
    }
    
    // Step 5: Signal and join GPU thread
    if (decoder->gpu_thread) {
        printf("av1_destroy_decoder: stopping GPU thread\n");
        if (decoder->gpu_thread->running) {
            pthread_cancel(decoder->gpu_thread->thread);
        }
        pthread_join(decoder->gpu_thread->thread, NULL);
        av1_mem_free(decoder->gpu_thread);
        decoder->gpu_thread = NULL;
    }
    
    // Step 6: Release DPB slots
    printf("av1_destroy_decoder: releasing DPB slots\n");
    for (int i = 0; i < decoder->dpb_count; i++) {
        if (decoder->dpb[i].in_use) {
            if (decoder->dpb[i].planes[0]) av1_mem_free(decoder->dpb[i].planes[0]);
            if (decoder->dpb[i].planes[1]) av1_mem_free(decoder->dpb[i].planes[1]);
            if (decoder->dpb[i].planes[2]) av1_mem_free(decoder->dpb[i].planes[2]);
            decoder->dpb[i].in_use = 0;
        }
    }
    
    // Step 7: Destroy queues
    av1_frame_queue_destroy(&decoder->output_queue);
    av1_frame_queue_destroy(&decoder->ready_queue);
    
    // Free queue storage
    if (decoder->queue_storage) {
        av1_mem_free(decoder->queue_storage);
        decoder->queue_storage = NULL;
    }
    
    // Step 8: Destroy synchronization primitives
    pthread_cond_destroy(&decoder->decoder_cond);
    pthread_mutex_destroy(&decoder->decoder_mutex);
    
    // Step 9: Destroy AOM decoder
    if (decoder->aom_decoder_initialized) {
        aom_codec_destroy(&decoder->aom_decoder);
        decoder->aom_decoder_initialized = false;
    }
    
    // Step 10: Zero the decoder struct (security wipe)
    void *decoder_ptr = decoder;
    size_t decoder_size = sizeof(Av1Decoder);
    memset(decoder_ptr, 0, decoder_size);
    
    // Step 11: Clear the memory pool flag (restore normal malloc)
    av1_mem_set_override_enabled(false);
    av1_mem_shutdown();
    
    // Free decoder structure using standard free (not av1_mem_free since we disabled override)
    free(decoder);
    
    printf("av1_destroy_decoder: complete\n");
    
    return 0;
}

Av1DecoderState av1_get_decoder_state(const Av1Decoder *decoder) {
    if (!decoder) {
        return AV1_DECODER_STATE_UNINITIALIZED;
    }
    return decoder->state;
}

const Av1DecoderConfig *av1_get_decoder_config(const Av1Decoder *decoder) {
    if (!decoder) {
        return NULL;
    }
    return &decoder->config;
}

Av1MemStats av1_get_mem_stats(const Av1Decoder *decoder) {
    Av1MemStats zero_stats = {0};
    if (!decoder) {
        return zero_stats;
    }
    return av1_mem_get_stats();
}

void av1_reset_mem_stats(Av1Decoder *decoder) {
    if (decoder) {
        av1_mem_reset_stats();
    }
}

// ============================================================================
// av1_decode Implementation
// ============================================================================

Av1DecodeResult av1_decode(Av1Decoder *decoder, 
                           const uint8_t *data, 
                           size_t data_size,
                           Av1DecodeOutput *out_result) {
    // Validate decoder and state
    if (!decoder) {
        fprintf(stderr, "av1_decode: NULL decoder\n");
        return AV1_INVALID_PARAM;
    }
    
    // Check if decoder is destroyed
    if (decoder->destroyed) {
        fprintf(stderr, "av1_decode: decoder is destroyed\n");
        return AV1_INVALID_PARAM;
    }
    
    if (!data || data_size == 0) {
        fprintf(stderr, "av1_decode: NULL data or zero size\n");
        return AV1_INVALID_PARAM;
    }
    
    // Check decoder state - reject if FLUSHING
    if (decoder->state == AV1_DECODER_STATE_FLUSHING) {
        fprintf(stderr, "av1_decode: decoder is flushing, rejecting new data\n");
        return AV1_FLUSHED;
    }
    
    // Check decoder state
    if (decoder->state != AV1_DECODER_STATE_CREATED &&
        decoder->state != AV1_DECODER_STATE_READY &&
        decoder->state != AV1_DECODER_STATE_DECODING) {
        fprintf(stderr, "av1_decode: invalid decoder state %d\n", decoder->state);
        return AV1_INVALID_PARAM;
    }
    
    // Set state to DECODING
    decoder->state = AV1_DECODER_STATE_DECODING;
    
    // Check if ready queue is full
    if (av1_frame_queue_is_full(&decoder->ready_queue)) {
        fprintf(stderr, "av1_decode: ready queue is full\n");
        decoder->state = AV1_DECODER_STATE_READY;
        return AV1_QUEUE_FULL;
    }
    
    // Call AOM decode
    aom_codec_err_t aom_err = aom_codec_decode(&decoder->aom_decoder, 
                                                 data, 
                                                 data_size, 
                                                 NULL);
    
    if (aom_err != AOM_CODEC_OK) {
        const char *error_msg = aom_codec_error(&decoder->aom_decoder);
        fprintf(stderr, "av1_decode: AOM decode error: %s\n", error_msg);
        decoder->decode_errors++;
        decoder->state = AV1_DECODER_STATE_ERROR;
        return AV1_ERROR;
    }
    
    // Process decoded frame(s)
    int frame_ready = 0;
    int show_existing = 0;
    int dpb_slot = -1;
    uint32_t frame_id = 0;
    
    // Get decoded frame from AOM
    aom_image_t *img = aom_codec_get_frame(&decoder->aom_decoder, &decoder->aom_decoder.iter);
    
    if (img) {
        frame_ready = 1;
        show_existing = 0;
        
        // Assign frame ID
        frame_id = decoder->next_frame_id++;
        
        // Allocate DPB slot
        dpb_slot = allocate_dpb_slot(decoder, img->w, img->h, img->bit_depth);
        if (dpb_slot < 0) {
            fprintf(stderr, "av1_decode: failed to allocate DPB slot\n");
            decoder->state = AV1_DECODER_STATE_READY;
            return AV1_ERROR;
        }
        
        // Copy frame data to DPB (simplified - real impl would use AOM's buffer)
        Av1DPBSlot *slot = &decoder->dpb[dpb_slot];
        
        // For now, use the AOM image directly if available
        if (img->planes[0]) {
            // Copy Y plane
            int y_size = img->stride[0] * img->h;
            memcpy(slot->planes[0], img->planes[0], y_size);
        }
        
        // Push to ready queue
        Av1FrameEntry entry = {
            .frame_id = frame_id,
            .dpb_slot = dpb_slot,
            .show_frame = 1,
            .show_existing_frame = show_existing
        };
        
        if (av1_frame_queue_push(&decoder->ready_queue, &entry) != 0) {
            fprintf(stderr, "av1_decode: failed to push to ready queue\n");
            release_dpb_slot(decoder, dpb_slot);
            decoder->state = AV1_DECODER_STATE_READY;
            return AV1_ERROR;
        }
        
        decoder->frames_decoded++;
        
        printf("av1_decode: decoded frame_id=%u, dpb_slot=%d, show=%d\n",
               frame_id, dpb_slot, entry.show_frame);
    } else {
        frame_ready = 0;
        printf("av1_decode: no frame output (not yet displayable)\n");
    }
    
    // Fill output result
    if (out_result) {
        out_result->frame_ready = frame_ready;
        out_result->frame_id = frame_id;
        out_result->show_existing_frame = show_existing;
        out_result->dpb_slot = dpb_slot;
    }
    
    // Set state back to READY
    decoder->state = AV1_DECODER_STATE_READY;
    
    return AV1_OK;
}

// Legacy function - now just calls av1_flush
Av1DecodeResult av1_decode_end(Av1Decoder *decoder) {
    return av1_flush(decoder);
}

// ============================================================================
// av1_sync Implementation
// ============================================================================

Av1DecodeResult av1_sync(Av1Decoder *decoder, 
                         uint32_t timeout_us,
                         Av1DecodeOutput *out_result) {
    if (!decoder) {
        return AV1_INVALID_PARAM;
    }
    
    // Check if destroyed
    if (decoder->destroyed) {
        return AV1_INVALID_PARAM;
    }
    
    // Check if we're in flushing state and queue is empty
    if (decoder->state == AV1_DECODER_STATE_FLUSHING) {
        if (av1_frame_queue_count(&decoder->ready_queue) == 0) {
            return AV1_END_OF_STREAM;
        }
    }
    
    // Pop from ready queue
    Av1FrameEntry entry;
    int result = av1_frame_queue_pop(&decoder->ready_queue, &entry, timeout_us);
    
    if (result != 0) {
        // Timeout or error
        if (decoder->state == AV1_DECODER_STATE_FLUSHING) {
            return AV1_END_OF_STREAM;
        }
        return AV1_NEED_MORE_DATA;
    }
    
    // Add to pending output table
    if (add_pending_output(decoder, entry.frame_id, entry.dpb_slot) != 0) {
        fprintf(stderr, "av1_sync: failed to add to pending output\n");
        // Put it back in queue
        av1_frame_queue_push(&decoder->ready_queue, &entry);
        return AV1_ERROR;
    }
    
    // Fill output result
    if (out_result) {
        out_result->frame_ready = 1;
        out_result->frame_id = entry.frame_id;
        out_result->show_existing_frame = entry.show_existing_frame;
        out_result->dpb_slot = entry.dpb_slot;
    }
    
    printf("av1_sync: frame_id=%u, dpb_slot=%d\n", entry.frame_id, entry.dpb_slot);
    
    return AV1_OK;
}

// ============================================================================
// av1_set_output Implementation
// ============================================================================

Av1DecodeResult av1_set_output(Av1Decoder *decoder, 
                               uint32_t frame_id,
                               const Av1OutputBuffer *output_buffer) {
    if (!decoder || !output_buffer) {
        return AV1_INVALID_PARAM;
    }
    
    // Check if destroyed
    if (decoder->destroyed) {
        return AV1_INVALID_PARAM;
    }
    
    // Find pending output entry
    Av1PendingOutput *pending = find_pending_output(decoder, frame_id);
    if (!pending) {
        fprintf(stderr, "av1_set_output: frame_id %u not found in pending output\n", frame_id);
        return AV1_ERROR;
    }
    
    // Get DPB slot
    int dpb_slot = pending->dpb_slot;
    if (dpb_slot < 0 || dpb_slot >= decoder->dpb_count) {
        fprintf(stderr, "av1_set_output: invalid dpb_slot %d\n", dpb_slot);
        return AV1_ERROR;
    }
    
    Av1DPBSlot *slot = &decoder->dpb[dpb_slot];
    if (!slot->in_use) {
        fprintf(stderr, "av1_set_output: DPB slot %d not in use\n", dpb_slot);
        return AV1_ERROR;
    }
    
    // Allocate copy job
    Av1CopyJob *copy_job = (Av1CopyJob *)malloc(sizeof(Av1CopyJob));
    if (!copy_job) {
        fprintf(stderr, "av1_set_output: failed to allocate copy job\n");
        return AV1_ERROR;
    }
    
    // Build copy job from DPB slot to output buffer
    memset(copy_job, 0, sizeof(Av1CopyJob));
    
    copy_job->frame_id = frame_id;
    copy_job->dpb_slot = dpb_slot;
    
    // Source (DPB)
    copy_job->src_planes[0] = slot->planes[0];
    copy_job->src_planes[1] = slot->planes[1];
    copy_job->src_planes[2] = slot->planes[2];
    copy_job->src_strides[0] = slot->strides[0];
    copy_job->src_strides[1] = slot->strides[1];
    copy_job->src_strides[2] = slot->strides[2];
    
    // Destination (output buffer)
    copy_job->dst_planes[0] = output_buffer->planes[0];
    copy_job->dst_planes[1] = output_buffer->planes[1];
    copy_job->dst_planes[2] = output_buffer->planes[2];
    copy_job->dst_strides[0] = output_buffer->strides[0];
    copy_job->dst_strides[1] = output_buffer->strides[1];
    copy_job->dst_strides[2] = output_buffer->strides[2];
    
    // Dimensions
    copy_job->plane_widths[0] = output_buffer->widths[0];
    copy_job->plane_widths[1] = output_buffer->widths[1];
    copy_job->plane_widths[2] = output_buffer->widths[2];
    copy_job->plane_heights[0] = output_buffer->heights[0];
    copy_job->plane_heights[1] = output_buffer->heights[1];
    copy_job->plane_heights[2] = output_buffer->heights[2];
    
    // Enqueue to copy thread
    if (av1_copy_thread_enqueue(decoder->copy_thread, copy_job) != 0) {
        fprintf(stderr, "av1_set_output: failed to enqueue copy job\n");
        free(copy_job);
        return AV1_ERROR;
    }
    
    // Store copy job in pending output
    pending->copy_job = copy_job;
    pending->ref_count++;
    
    printf("av1_set_output: frame_id=%u, enqueued copy job\n", frame_id);
    
    return AV1_OK;
}

// ============================================================================
// av1_receive_output Implementation
// ============================================================================

Av1DecodeResult av1_receive_output(Av1Decoder *decoder, 
                                    uint32_t frame_id,
                                    uint32_t timeout_us) {
    if (!decoder) {
        return AV1_INVALID_PARAM;
    }
    
    // Check if destroyed
    if (decoder->destroyed) {
        return AV1_INVALID_PARAM;
    }
    
    // Find pending output entry
    Av1PendingOutput *pending = find_pending_output(decoder, frame_id);
    if (!pending) {
        fprintf(stderr, "av1_receive_output: frame_id %u not found\n", frame_id);
        return AV1_ERROR;
    }
    
    // If no copy job (frame not set for output), just release DPB
    if (!pending->copy_job) {
        // Release DPB reference
        release_dpb_slot(decoder, pending->dpb_slot);
        remove_pending_output(decoder, frame_id);
        return AV1_OK;
    }
    
    // Wait for copy job to complete
    int wait_result = av1_copy_thread_wait(decoder->copy_thread, pending->copy_job, timeout_us);
    
    if (wait_result != 0) {
        fprintf(stderr, "av1_receive_output: timeout waiting for copy job\n");
        return AV1_ERROR;
    }
    
    // Check copy status
    int status = av1_copy_thread_get_status(pending->copy_job);
    if (status != AV1_COPY_COMPLETE) {
        fprintf(stderr, "av1_receive_output: copy job failed (status=%d)\n", status);
        return AV1_ERROR;
    }
    
    // Release DPB reference
    release_dpb_slot(decoder, pending->dpb_slot);
    
    // Free copy job
    free(pending->copy_job);
    pending->copy_job = NULL;
    
    // Remove from pending output table
    remove_pending_output(decoder, frame_id);
    
    printf("av1_receive_output: frame_id=%u completed\n", frame_id);
    
    return AV1_OK;
}

// Legacy functions
Av1DecodeResult av1_get_decoded_frame(Av1Decoder *decoder, 
                                       Av1FrameEntry *out_entry,
                                       uint32_t timeout_us) {
    if (!decoder || !out_entry) {
        return AV1_INVALID_PARAM;
    }
    
    if (decoder->destroyed) {
        return AV1_INVALID_PARAM;
    }
    
    int result = av1_frame_queue_pop(&decoder->ready_queue, out_entry, timeout_us);
    
    if (result == 0) {
        decoder->frames_output++;
        return AV1_OK;
    }
    
    return AV1_ERROR;
}

Av1DecodeResult av1_release_frame(Av1Decoder *decoder, uint32_t frame_id) {
    if (!decoder) {
        return AV1_INVALID_PARAM;
    }
    
    if (decoder->destroyed) {
        return AV1_INVALID_PARAM;
    }
    
    // Find and release pending output
    Av1PendingOutput *pending = find_pending_output(decoder, frame_id);
    if (pending) {
        release_dpb_slot(decoder, pending->dpb_slot);
        if (pending->copy_job) {
            free(pending->copy_job);
        }
        remove_pending_output(decoder, frame_id);
    }
    
    return AV1_OK;
}
```

Now let me create the Makefile:

### Makefile

```makefile
# AV1 Decoder Makefile
# 
# Targets:
#   test_mem      - Standalone memory override test
#   test_queue    - Standalone job queue test
#   test_copy     - Standalone copy thread test
#   test_gpu      - Standalone GPU thread stub test
#   test_e2e      - Full end-to-end test linking everything + libaom
#   all           - Build everything
#   clean         - Remove build artifacts

CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -pthread -I.
LDFLAGS_E2E = -laom -lm

# Source files
MEM_OVERRIDE_SRC = av1_mem_override.c
JOB_QUEUE_SRC = av1_job_queue.c
COPY_THREAD_SRC = av1_copy_thread.c
GPU_THREAD_SRC = av1_gpu_thread.c
IVF_PARSER_SRC = ivf_parser.c
Y4M_WRITER_SRC = y4m_writer.c
DECODER_API_SRC = av1_decoder_api.c

# Object files
MEM_OVERRIDE_OBJ = av1_mem_override.o
JOB_QUEUE_OBJ = av1_job_queue.o
COPY_THREAD_OBJ = av1_copy_thread.o
GPU_THREAD_OBJ = av1_gpu_thread.o
IVF_PARSER_OBJ = ivf_parser.o
Y4M_WRITER_OBJ = y4m_writer.o
DECODER_API_OBJ = av1_decoder_api.o

# Test executables
TEST_MEM = test_mem
TEST_QUEUE = test_queue
TEST_COPY = test_copy
TEST_GPU = test_gpu
TEST_E2E = test_e2e

# Default target
all: $(TEST_MEM) $(TEST_QUEUE) $(TEST_COPY) $(TEST_GPU) $(TEST_E2E)

# Test: Memory override
$(TEST_MEM): tests/test_mem_override.c $(MEM_OVERRIDE_OBJ)
	$(CC) $(CFLAGS) -o $@ tests/test_mem_override.c $(MEM_OVERRIDE_OBJ) $(LDFLAGS_E2E)

# Test: Job queue
$(TEST_QUEUE): tests/test_job_queue.c $(JOB_QUEUE_OBJ)
	$(CC) $(CFLAGS) -o $@ tests/test_job_queue.c $(JOB_QUEUE_OBJ) -lpthread

# Test: Copy thread
$(TEST_COPY): tests/test_copy_thread.c $(COPY_THREAD_OBJ) $(JOB_QUEUE_OBJ)
	$(CC) $(CFLAGS) -o $@ tests/test_copy_thread.c $(COPY_THREAD_OBJ) $(JOB_QUEUE_OBJ) -lpthread

# Test: GPU thread
$(TEST_GPU): tests/test_gpu_thread.c $(GPU_THREAD_OBJ) $(JOB_QUEUE_OBJ)
	$(CC) $(CFLAGS) -o $@ tests/test_gpu_thread.c $(GPU_THREAD_OBJ) $(JOB_QUEUE_OBJ) -lpthread

# Test: End-to-end
$(TEST_E2E): tests/test_e2e.c $(DECODER_API_OBJ) $(MEM_OVERRIDE_OBJ) \
             $(JOB_QUEUE_OBJ) $(COPY_THREAD_OBJ) $(GPU_THREAD_OBJ) \
             $(IVF_PARSER_OBJ) $(Y4M_WRITER_OBJ)
	$(CC) $(CFLAGS) -o $@ tests/test_e2e.c \
		$(DECODER_API_OBJ) $(MEM_OVERRIDE_OBJ) \
		$(JOB_QUEUE_OBJ) $(COPY_THREAD_OBJ) $(GPU_THREAD_OBJ) \
		$(IVF_PARSER_OBJ) $(Y4M_WRITER_OBJ) $(LDFLAGS_E2E)

# Object file rules
$(MEM_OVERRIDE_OBJ): $(MEM_OVERRIDE_SRC) av1_mem_override.h
	$(CC) $(CFLAGS) -c -o $@ $(MEM_OVERRIDE_SRC)

$(JOB_QUEUE_OBJ): $(JOB_QUEUE_SRC) av1_job_queue.h
	$(CC) $(CFLAGS) -c -o $@ $(JOB_QUEUE_SRC)

$(COPY_THREAD_OBJ): $(COPY_THREAD_SRC) av1_copy_thread.h
	$(CC) $(CFLAGS) -c -o $@ $(COPY_THREAD_SRC)

$(GPU_THREAD_OBJ): $(GPU_THREAD_SRC) av1_gpu_thread.h
	$(CC) $(CFLAGS) -c -o $@ $(GPU_THREAD_SRC)

$(IVF_PARSER_OBJ): $(IVF_PARSER_SRC) ivf_parser.h
	$(CC) $(CFLAGS) -c -o $@ $(IVF_PARSER_SRC)

$(Y4M_WRITER_OBJ): $(Y4M_WRITER_SRC) y4m_writer.h
	$(CC) $(CFLAGS) -c -o $@ $(Y4M_WRITER_SRC)

$(DECODER_API_OBJ): $(DECODER_API_SRC) av1_decoder_api.h av1_mem_override.h \
                    av1_copy_thread.h av1_job_queue.h
	$(CC) $(CFLAGS) -c -o $@ $(DECODER_API_SRC)

# Clean target
clean:
	rm -f $(TEST_MEM) $(TEST_QUEUE) $(TEST_COPY) $(TEST_GPU) $(TEST_E2E)
	rm -f $(MEM_OVERRIDE_OBJ) $(JOB_QUEUE_OBJ) $(COPY_THREAD_OBJ) \
	      $(GPU_THREAD_OBJ) $(IVF_PARSER_OBJ) $(Y4M_WRITER_OBJ) \
	      $(DECODER_API_OBJ)
	rm -f *.o tests/*.o

# Phony targets
.PHONY: all clean
```

Now let me provide all the remaining header and source files that need to be in the tree:

### av1_copy_thread.h

```c
#ifndef AV1_COPY_THREAD_H
#define AV1_COPY_THREAD_H

#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>

/* Copy job status values */
#define AV1_COPY_PENDING    0
#define AV1_COPY_IN_PROGRESS 1
#define AV1_COPY_COMPLETE   2

/* Maximum number of copy jobs in the queue */
#define AV1_COPY_QUEUE_DEPTH 8

typedef struct Av1CopyJob {
    uint32_t frame_id;
    int      dpb_slot;
    
    /* Source (internal DPB frame buffer) */
    const uint8_t *src_planes[3];
    int            src_strides[3];
    
    /* Destination (caller-provided buffer) */
    uint8_t       *dst_planes[3];
    int            dst_strides[3];
    
    /* Dimensions (width in bytes per plane, rows per plane) */
    int            plane_widths[3];
    int            plane_heights[3];
    
    /* Status - atomic for thread-safe access */
    _Atomic int    status;
} Av1CopyJob;

/* Opaque copy thread handle */
typedef struct Av1CopyThread Av1CopyThread;

/**
 * Create and start a copy thread.
 * @param queue_depth Number of jobs that can be queued (max)
 * @return Pointer to copy thread handle, or NULL on error
 */
Av1CopyThread* av1_copy_thread_create(int queue_depth);

/**
 * Enqueue a copy job to the copy thread.
 * @param ct   Copy thread handle
 * @param job  Copy job to enqueue
 * @return 0 on success, -1 on error (queue full or invalid args)
 */
int av1_copy_thread_enqueue(Av1CopyThread *ct, Av1CopyJob *job);

/**
 * Wait for a specific copy job to complete.
 * @param ct   Copy thread handle
 * @param job  Copy job to wait on
 * @param timeout_us Timeout in microseconds. 0 = infinite wait.
 * @return 0 on success (job completed), -1 on timeout or error
 */
int av1_copy_thread_wait(Av1CopyThread *ct, Av1CopyJob *job, uint32_t timeout_us);

/**
 * Get the current status of a copy job.
 * @param job  Copy job
 * @return Current status (AV1_COPY_PENDING, AV1_COPY_IN_PROGRESS, AV1_COPY_COMPLETE)
 */
int av1_copy_thread_get_status(Av1CopyJob *job);

/**
 * Destroy the copy thread (graceful shutdown).
 * Waits for any pending jobs to complete, then terminates the thread.
 * @param ct   Copy thread handle
 * @return 0 on success, -1 on error
 */
int av1_copy_thread_destroy(Av1CopyThread *ct);

/* GPU extension point - stub for future GPU offload */
#define AV1_COPY_FLAG_GPU 0x01

/**
 * Enqueue a copy job with options (GPU extension point).
 * @param ct     Copy thread handle
 * @param job    Copy job to enqueue
 * @param flags  Option flags (AV1_COPY_FLAG_GPU for GPU copy)
 * @return 0 on success, -1 on error
 */
// GPU_IMPL: int av1_copy_thread_enqueue_ex(Av1CopyThread *ct, Av1CopyJob *job, int flags);

#endif /* AV1_COPY_THREAD_H */
```

### av1_copy_thread.c

```c
#include "av1_copy_thread.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <stdio.h>

#define NSEC_PER_USEC 1000ULL
#define USEC_PER_SEC  1000000ULL

/* Internal queue entry */
typedef struct Av1CopyQueueEntry {
    Av1CopyJob *job;
    int         enqueued;  /* 1 if entry contains a valid job */
} Av1CopyQueueEntry;

struct Av1CopyThread {
    Av1CopyQueueEntry *queue;      /* circular buffer */
    int                capacity;   /* max queue entries */
    int                head;       /* next to dequeue */
    int                tail;       /* next to enqueue */
    int                count;      /* current jobs in queue */
    
    pthread_mutex_t    mutex;
    pthread_cond_t     job_available;  /* signaled when job is enqueued */
    pthread_cond_t     job_complete;   /* signaled when job completes */
    
    pthread_t          thread;
    int                running;    /* 1 while thread should run */
    int                shutdown;   /* 1 when shutdown requested */
};

/* Forward declarations */
static void* copy_thread_main(void *arg);
static void copy_job_execute(Av1CopyJob *job);

Av1CopyThread* av1_copy_thread_create(int queue_depth) {
    if (queue_depth <= 0) {
        queue_depth = AV1_COPY_QUEUE_DEPTH;
    }
    
    Av1CopyThread *ct = calloc(1, sizeof(Av1CopyThread));
    if (!ct) {
        return NULL;
    }
    
    ct->queue = calloc(queue_depth, sizeof(Av1CopyQueueEntry));
    if (!ct->queue) {
        free(ct);
        return NULL;
    }
    
    ct->capacity = queue_depth;
    ct->head = 0;
    ct->tail = 0;
    ct->count = 0;
    ct->running = 1;
    ct->shutdown = 0;
    
    /* Initialize synchronization primitives */
    if (pthread_mutex_init(&ct->mutex, NULL) != 0) {
        free(ct->queue);
        free(ct);
        return NULL;
    }
    
    if (pthread_cond_init(&ct->job_available, NULL) != 0) {
        pthread_mutex_destroy(&ct->mutex);
        free(ct->queue);
        free(ct);
        return NULL;
    }
    
    if (pthread_cond_init(&ct->job_complete, NULL) != 0) {
        pthread_cond_destroy(&ct->job_available);
        pthread_mutex_destroy(&ct->mutex);
        free(ct->queue);
        free(ct);
        return NULL;
    }
    
    /* Start the copy thread */
    if (pthread_create(&ct->thread, NULL, copy_thread_main, ct) != 0) {
        pthread_cond_destroy(&ct->job_complete);
        pthread_cond_destroy(&ct->job_available);
        pthread_mutex_destroy(&ct->mutex);
        free(ct->queue);
        free(ct);
        return NULL;
    }
    
    return ct;
}

int av1_copy_thread_enqueue(Av1CopyThread *ct, Av1CopyJob *job) {
    if (!ct || !job) {
        return -1;
    }
    
    /* Initialize job status */
    atomic_store(&job->status, AV1_COPY_PENDING);
    
    pthread_mutex_lock(&ct->mutex);
    
    /* Check if queue is full */
    if (ct->count >= ct->capacity) {
        pthread_mutex_unlock(&ct->mutex);
        return -1;
    }
    
    /* Enqueue the job */
    ct->queue[ct->tail].job = job;
    ct->queue[ct->tail].enqueued = 1;
    ct->tail = (ct->tail + 1) % ct->capacity;
    ct->count++;
    
    /* Signal the copy thread that a job is available */
    pthread_cond_signal(&ct->job_available);
    
    pthread_mutex_unlock(&ct->mutex);
    
    return 0;
}

int av1_copy_thread_wait(Av1CopyThread *ct, Av1CopyJob *job, uint32_t timeout_us) {
    if (!ct || !job) {
        return -1;
    }
    
    pthread_mutex_lock(&ct->mutex);
    
    /* Check current status without waiting */
    int current_status = atomic_load(&job->status);
    if (current_status == AV1_COPY_COMPLETE) {
        pthread_mutex_unlock(&ct->mutex);
        return 0;
    }
    
    if (current_status == AV1_COPY_IN_PROGRESS) {
        /* Job is being processed, wait for completion */
        if (timeout_us == 0) {
            /* Infinite wait */
            while (atomic_load(&job->status) == AV1_COPY_IN_PROGRESS