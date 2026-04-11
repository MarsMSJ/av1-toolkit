

I'll implement the output retrieval pipeline with the pending output table, sync/set_output/receive_output functions, and the Y4M writer helper.

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
    AV1_END_OF_STREAM = -5
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
 * Destroy a decoder instance and release all resources.
 * 
 * @param decoder Decoder handle to destroy
 * @return 0 on success, -1 on error
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
 *         queue is full, AV1_INVALID_PARAM if decoder state is invalid
 */
Av1DecodeResult av1_decode(Av1Decoder *decoder, 
                           const uint8_t *data, 
                           size_t data_size,
                           Av1DecodeOutput *out_result);

/**
 * Signal end of stream to the decoder.
 * 
 * After calling this, the decoder will enter FLUSHING state and will
 * return AV1_END_OF_STREAM from av1_sync() when all frames have been
 * output.
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

// Include the memory override and job queue
#include "av1_mem_override.h"
#include "av1_job_queue.h"

// ============================================================================
// AOM Integration
// ============================================================================

// Include AOM headers for decoder integration
#include "aom/aom_decoder.h"
#include "aom/aomdx.h"
#include "av1/av1_dx.h"

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
    aom_codec_err_t aom_err = aom_codec_init(&av1_codec_algo, &aom_config, &decoder->aom_decoder);
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

int av1_destroy_decoder(Av1Decoder *decoder) {
    if (!decoder) {
        return -1;
    }
    
    // Set state to indicate shutdown
    decoder->state = AV1_DECODER_STATE_UNINITIALIZED;
    
    // Stop and join worker threads
    if (decoder->workers) {
        for (int i = 0; i < decoder->num_workers; i++) {
            if (decoder->workers[i].running) {
                pthread_cancel(decoder->workers[i].thread);
            }
            pthread_join(decoder->workers[i].thread, NULL);
        }
        av1_mem_free(decoder->workers);
        decoder->workers = NULL;
    }
    
    // Stop and join GPU thread
    if (decoder->gpu_thread) {
        if (decoder->gpu_thread->running) {
            pthread_cancel(decoder->gpu_thread->thread);
        }
        pthread_join(decoder->gpu_thread->thread, NULL);
        av1_mem_free(decoder->gpu_thread);
        decoder->gpu_thread = NULL;
    }
    
    // Destroy copy thread
    if (decoder->copy_thread) {
        av1_copy_thread_destroy(decoder->copy_thread);
        decoder->copy_thread = NULL;
    }
    
    // Release DPB slots
    for (int i = 0; i < decoder->dpb_count; i++) {
        if (decoder->dpb[i].in_use) {
            if (decoder->dpb[i].planes[0]) av1_mem_free(decoder->dpb[i].planes[0]);
            if (decoder->dpb[i].planes[1]) av1_mem_free(decoder->dpb[i].planes[1]);
            if (decoder->dpb[i].planes[2]) av1_mem_free(decoder->dpb[i].planes[2]);
        }
    }
    
    // Destroy queues
    av1_frame_queue_destroy(&decoder->output_queue);
    av1_frame_queue_destroy(&decoder->ready_queue);
    
    // Free queue storage
    if (decoder->queue_storage) {
        av1_mem_free(decoder->queue_storage);
    }
    
    // Destroy synchronization primitives
    pthread_cond_destroy(&decoder->decoder_cond);
    pthread_mutex_destroy(&decoder->decoder_mutex);
    
    // Destroy AOM decoder
    if (decoder->aom_decoder_initialized) {
        aom_codec_destroy(&decoder->aom_decoder);
        decoder->aom_decoder_initialized = false;
    }
    
    // Free decoder structure
    av1_mem_free(decoder);
    
    // Shutdown memory allocator
    av1_mem_set_override_enabled(false);
    av1_mem_shutdown();
    
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
    
    if (!data || data_size == 0) {
        fprintf(stderr, "av1_decode: NULL data or zero size\n");
        return AV1_INVALID_PARAM;
    }
    
    // Check decoder state
    if (decoder->state != AV1_DECODER_STATE_CREATED &&
        decoder->state != AV1_DECODER_STATE_READY &&
        decoder->state != AV1_DECODER_STATE_DECODING &&
        decoder->state != AV1_DECODER_STATE_FLUSHING) {
        fprintf(stderr, "av1_decode: invalid decoder state %d\n", decoder->state);
        return AV1_INVALID_PARAM;
    }
    
    // Set state to DECODING
    decoder->state = AV1_DECODER_STATE_DECODING;
    
    // Check if ready queue is full
    if (av1_frame_queue_is_full(&decoder->ready_queue)) {
        fprintf(stderr, "av1_decode: ready queue is full\n");
        decoder->state = (decoder->state == AV1_DECODER_STATE_FLUSHING) ? 
                         AV1_DECODER_STATE_FLUSHING : AV1_DECODER_STATE_READY;
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
    
    // Set state back to READY or FLUSHING
    decoder->state = (decoder->state == AV1_DECODER_STATE_FLUSHING) ? 
                     AV1_DECODER_STATE_FLUSHING : AV1_DECODER_STATE_READY;
    
    return AV1_OK;
}

Av1DecodeResult av1_decode_end(Av1Decoder *decoder) {
    if (!decoder) {
        return AV1_INVALID_PARAM;
    }
    
    // Signal end of stream
    decoder->state = AV1_DECODER_STATE_FLUSHING;
    
    // Flush AOM decoder to get remaining frames
    aom_codec_err_t aom_err = aom_codec_decode(&decoder->aom_decoder, NULL, 0, NULL);
    if (aom_err != AOM_CODEC_OK) {
        // Ignore flush errors
    }
    
    // Try to get any remaining frames
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
    
    return AV1_OK;
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

### y4m_writer.h
```c
#ifndef Y4M_WRITER_H
#define Y4M_WRITER_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Y4M Writer Handle
// ============================================================================

typedef struct Y4MWriter Y4MWriter;

// ============================================================================
// Y4M Colorspace Types
// ============================================================================

#define Y4M_COLORSPACE_YUV420 "420"
#define Y4M_COLORSPACE_YUV422 "422"
#define Y4M_COLORSPACE_YUV444 "444"
#define Y4M_COLORSPACE_YUV420P10 "420p10"
#define Y4M_COLORSPACE_YUV420P12 "420p12"

// ============================================================================
// API Functions
// ============================================================================

/**
 * Open a Y4M file for writing.
 * 
 * @param filename Path to output Y4M file
 * @param width    Frame width in pixels
 * @param height   Frame height in pixels
 * @param fps_n    Frame rate numerator (e.g., 30)
 * @param fps_d    Frame rate denominator (e.g., 1)
 * @param bit_depth Bit depth (8, 10, 12)
 * @param chroma_subsampling Chroma subsampling (0=420, 1=422, 2=444)
 * @return Writer handle on success, NULL on failure
 */
Y4MWriter *y4m_writer_open(const char *filename, 
                            int width, 
                            int height,
                            int fps_n, 
                            int fps_d,
                            int bit_depth,
                            int chroma_subsampling);

/**
 * Close a Y4M file and free resources.
 * 
 * @param writer Writer handle
 * @return 0 on success, -1 on error
 */
int y4m_writer_close(Y4MWriter *writer);

/**
 * Write a frame to the Y4M file.
 * 
 * @param writer Writer handle
 * @param y      Y plane data
 * @param u      U plane data
 * @param v      V plane data
 * @param y_stride Y plane stride (bytes per row)
 * @param u_stride U plane stride
 * @param v_stride V plane stride
 * @return 0 on success, -1 on error
 */
int y4m_writer_write_frame(Y4MWriter *writer,
                            const uint8_t *y,
                            const uint8_t *u,
                            const uint8_t *v,
                            int y_stride,
                            int u_stride,
                            int v_stride);

/**
 * Write a frame from an Av1OutputBuffer.
 * 
 * @param writer Writer handle
 * @param buffer Output buffer with frame data
 * @return 0 on success, -1 on error
 */
int y4m_writer_write_buffer(Y4MWriter *writer, 
                             const Av1OutputBuffer *buffer);

/**
 * Get the number of frames written.
 * 
 * @param writer Writer handle
 * @return Number of frames written
 */
uint64_t y4m_writer_get_frame_count(const Y4MWriter *writer);

/**
 * Check if the writer is valid and open.
 * 
 * @param writer Writer handle
 * @return true if valid, false otherwise
 */
bool y4m_writer_is_valid(const Y4MWriter *writer);

#ifdef __cplusplus
}
#endif

#endif // Y4M_WRITER_H
```

### y4m_writer.c
```c
#include "y4m_writer.h"
#include "av1_decoder_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// ============================================================================
// Internal Structures
// ============================================================================

struct Y4MWriter {
    FILE *file;
    int width;
    int height;
    int fps_n;
    int fps_d;
    int bit_depth;
    int chroma_subsampling;
    uint64_t frame_count;
    bool valid;
};

// ============================================================================
// Internal Functions
// ============================================================================

static const char* get_colorspace(int chroma_subsampling, int bit_depth) {
    if (bit_depth > 8) {
        switch (chroma_subsampling) {
            case 0: return "420p10";
            case 1: return "422p10";
            case 2: return "444p10";
            default: return "420p10";
        }
    } else {
        switch (chroma_subsampling) {
            case 0: return "420";
            case 1: return "422";
            case 2: return "444";
            default: return "420";
        }
    }
}

// ============================================================================
// API Implementation
// ============================================================================

Y4MWriter *y4m_writer_open(const char *filename, 
                            int width, 
                            int height,
                            int fps_n, 
                            int fps_d,
                            int bit_depth,
                            int chroma_subsampling) {
    if (!filename || width <= 0 || height <= 0) {
        fprintf(stderr, "y4m_writer_open: invalid parameters\n");
        return NULL;
    }
    
    // Allocate writer
    Y4MWriter *writer = (Y4MWriter *)calloc(1, sizeof(Y4MWriter));
    if (!writer) {
        fprintf(stderr, "y4m_writer_open: failed to allocate writer\n");
        return NULL;
    }
    
    // Open file
    writer->file = fopen(filename, "wb");
    if (!writer->file) {
        fprintf(stderr, "y4m_writer_open: failed to open file: %s\n", filename);
        free(writer);
        return NULL;
    }
    
    // Store parameters
    writer->width = width;
    writer->height = height;
    writer->fps_n = fps_n > 0 ? fps_n : 30;
    writer->fps_d = fps_d > 0 ? fps_d : 1;
    writer->bit_depth = bit_depth > 0 ? bit_depth : 8;
    writer->chroma_subsampling = chroma_subsampling;
    writer->frame_count = 0;
    writer->valid = true;
    
    // Write Y4M header
    const char *colorspace = get_colorspace(chroma_subsampling, writer->bit_depth);
    fprintf(writer->file, "YUV4MPEG2 W%d H%d F%d:%d C%s\n",
            width, height, writer->fps_n, writer->fps_d, colorspace);
    
    printf("Y4M file opened: %s (%dx%d, %s, %dfps)\n", 
           filename, width, height, colorspace, writer->fps_n);
    
    return writer;
}

int y4m_writer_close(Y4MWriter *writer) {
    if (!writer) {
        return -1;
    }
    
    if (writer->file) {
        fclose(writer->file);
    }
    
    printf("Y4M file closed: %lu frames written\n", 
           (unsigned long)writer->frame_count);
    
    free(writer);
    return 0;
}

int y4m_writer_write_frame(Y4MWriter *writer,
                            const uint8_t *y,
                            const uint8_t *u,
                            const uint8_t *v,
                            int y_stride,
                            int u_stride,
                            int v_stride) {
    if (!writer || !writer->valid || !y || !u || !v) {
        return -1;
    }
    
    // Write frame header
    fprintf(writer->file, "FRAME\n");
    
    // Calculate plane dimensions
    int y_width = writer->width;
    int y_height = writer->height;
    int uv_width = writer->width;
    int uv_height = writer->height;
    
    switch (writer->chroma_subsampling) {
        case 0: // 420
            uv_width = writer->width / 2;
            uv_height = writer->height / 2;
            break;
        case 1: // 422
            uv_width = writer->width / 2;
            break;
        case 2: // 444
            break;
        default:
            uv_width = writer->width / 2;
            uv_height = writer->height / 2;
            break;
    }
    
    int bytes_per_sample = (writer->bit_depth > 8) ? 2 : 1;
    
    // Write Y plane
    for (int row = 0; row < y_height; row++) {
        const uint8_t *row_data = y + (row * y_stride);
        if (fwrite(row_data, bytes_per_sample, y_width, writer->file) != (size_t)y_width) {
            return -1;
        }
    }
    
    // Write U plane
    for (int row = 0; row < uv_height; row++) {
        const uint8_t *row_data = u + (row * u_stride);
        if (fwrite(row_data, bytes_per_sample, uv_width, writer->file) != (size_t)uv_width) {
            return -1;
        }
    }
    
    // Write V plane
    for (int row = 0; row < uv_height; row++) {
        const uint8_t *row_data = v + (row * v_stride);
        if (fwrite(row_data, bytes_per_sample, uv_width, writer->file) != (size_t)uv_width) {
            return -1;
        }
    }
    
    writer->frame_count++;
    
    return 0;
}

int y4m_writer_write_buffer(Y4MWriter *writer, 
                             const Av1OutputBuffer *buffer) {
    if (!writer || !buffer || !buffer->planes[0]) {
        return -1;
    }
    
    return y4m_writer_write_frame(writer,
                                   buffer->planes[0],
                                   buffer->planes[1],
                                   buffer->planes[2],
                                   buffer->strides[0],
                                   buffer->strides[1],
                                   buffer->strides[2]);
}

uint64_t y4m_writer_get_frame_count(const Y4MWriter *writer) {
    if (!writer) {
        return 0;
    }
    return writer->frame_count;
}

bool y4m_writer_is_valid(const Y4MWriter *writer) {
    return writer && writer->valid;
}
```

### test_sync_output.c
```c
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