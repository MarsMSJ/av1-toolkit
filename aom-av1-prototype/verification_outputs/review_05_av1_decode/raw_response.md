

## Code Review: AV1 Decoder - Serial Decode with AU = TD-to-TD

### Requirements Checklist

| # | Requirement | Status | Explanation |
|---|-------------|--------|-------------|
| 1 | Decodes AV1 bitstream without crashing | **PASS** | Uses AOM decoder; proper initialization and cleanup |
| 2 | Frame ready flag set correctly for show_frame / show_existing | **FAIL** | Always sets `show_frame = 1` regardless of actual frame properties |
| 3 | Non-display frames (reference only) produce frame_ready=0 | **FAIL** | Always sets `frame_ready = 1` when AOM returns a frame |
| 4 | QUEUE_FULL returned when ready queue is full | **PASS** | Correctly checks queue before decoding |
| 5 | Frame IDs are monotonically increasing | **PASS** | Uses `decoder->next_frame_id++` correctly |
| 6 | State transitions correct (CREATED -> DECODING) | **PASS** | State changes from CREATED/READY to DECODING, then back to READY |

---

### Issues Found

#### 1. CRITICAL: Incorrect show_frame handling
- **File**: `av1_decoder_api.c`, function `av1_decode()` (~line 580)
- **What's wrong**: The code always sets `show_frame = 1` when a frame is decoded, regardless of the actual AV1 frame properties. It doesn't query the AOM decoder for the actual show_frame flag.
- **How to fix**: Use `aom_codec_control()` with `AOMD_GET_FRAME_FLAGS` to retrieve the frame flags from AOM, or parse the OBU to determine if the frame should be displayed.

#### 2. CRITICAL: Incorrect frame_ready for reference frames
- **File**: `av1_decoder_api.c`, function `av1_decode()` (~line 560)
- **What's wrong**: When `img` is not NULL, `frame_ready` is always set to 1. Reference-only frames (show_frame=0) should produce `frame_ready=0`.
- **How to fix**: Check the frame flags from AOM to determine if the frame should be displayed before setting `frame_ready = 1`.

#### 3. WARNING: Uninitialized iterator
- **File**: `av1_decoder_api.c`, function `av1_decode()` (~line 545)
- **What's wrong**: Uses `decoder->aom_decoder.iter` without initialization. The iterator should be initialized to NULL before first use.
- **How to fix**: Add `decoder->aom_decoder.iter = NULL;` before the decode call or use a local iterator variable.

#### 4. WARNING: Typo in test file
- **File**: `test_av1_decode.c` (~line 170)
- **What's wrong**: Uses `entry.dpbslot` but the field is named `entry.dpb_slot`.
- **How to fix**: Change `entry.dpbslot` to `entry.dpb_slot`.

#### 5. STYLE: Missing AOM library reference check
- **File**: `av1_decoder_api.c` (~line 30)
- **What's wrong**: Uses `av1_codec_algo` without verifying it's properly defined. This is an external symbol from the AOM library.
- **How to fix**: Ensure the AOM library is properly linked and the symbol is declared in the header.

---

### CORRECTED FILES

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

typedef struct Av1CopyThread {
    pthread_t thread;
    Av1ThreadConfig config;
    bool running;
    Av1FrameQueue *input_queue;
    Av1FrameQueue *output_queue;
} Av1CopyThread;

typedef struct Av1GPUThread {
    pthread_t thread;
    Av1ThreadConfig config;
    bool running;
    int gpu_device;
    void *film_grain_buffer;  // For future GPU film grain
} Av1GPUThread;

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
    
    // Worker threads
    Av1WorkerThread *workers;
    int num_workers;
    
    // Copy thread
    Av1CopyThread *copy_thread;
    
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

// ============================================================================
// Worker Thread Implementation (Stub)
// ============================================================================

static void *worker_thread_func(void *arg) {
    Av1WorkerThread *worker = (Av1WorkerThread *)arg;
    
    printf("Worker thread %d started\n", worker->thread_id);
    
    // Set thread priority/affinity
    set_thread_priority(worker->thread, &worker->config);
    
    // Stub: In a real implementation, this would process decode jobs
    // For now, just mark as running
    worker->running = true;
    
    // Stub: Wait for work (not implemented)
    // In real implementation, would wait on a job queue
    
    printf("Worker thread %d exiting\n", worker->thread_id);
    return NULL;
}

// ============================================================================
// Copy Thread Implementation (Stub)
// ============================================================================

static void *copy_thread_func(void *arg) {
    Av1CopyThread *copy = (Av1CopyThread *)arg;
    
    printf("Copy thread started\n");
    
    set_thread_priority(copy->thread, &copy->config);
    
    copy->running = true;
    
    // Stub: In real implementation, would copy decoded frames to output
    // Currently just a placeholder
    
    printf("Copy thread exiting\n");
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
    // (film grain synthesis, format conversion, etc.)
    
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
        .width = config->memory_size > 0 ? 1920 : 0,  // Default if not specified
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
    
    // Initialize AOM decoder
    memset(&decoder->aom_decoder, 0, sizeof(decoder->aom_decoder));
    
    // Configure AOM decoder
    aom_codec_dec_cfg_t aom_config = {
        .threads = config->num_worker_threads > 0 ? config->num_worker_threads : 1,
        .width = 0,  // Will be set from stream
        .height = 0,
        .alloc_buf = NULL,
        .alloc_buf_size = 0,
        .init_flags = 0
    };
    
    // Set threading flags
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
    
    // Allocate queue storage (two queues: ready and output)
    int queue_capacity = config->queue_depth + 4;  // Extra for safety
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
    
    // Allocate and create worker threads
    int num_workers = config->num_worker_threads > 0 ? config->num_worker_threads : 4;
    decoder->num_workers = num_workers;
    
    if (num_workers > 0) {
        decoder->workers = (Av1WorkerThread *)av1_mem_calloc(num_workers, sizeof(Av1WorkerThread));
        if (!decoder->workers) {
            fprintf(stderr, "av1_create_decoder: failed to allocate workers\n");
            goto cleanup_output_queue;
        }
        
        for (int i = 0; i < num_workers; i++) {
            decoder->workers[i].thread_id = i;
            decoder->workers[i].config = config->worker_config;
            decoder->workers[i].running = false;
            
            if (pthread_create(&decoder->workers[i].thread, NULL, 
                               worker_thread_func, &decoder->workers[i]) != 0) {
                fprintf(stderr, "av1_create_decoder: failed to create worker %d\n", i);
                // Clean up created workers
                for (int j = 0; j < i; j++) {
                    pthread_cancel(decoder->workers[j].thread);
                    pthread_join(decoder->workers[j].thread, NULL);
                }
                av1_mem_free(decoder->workers);
                decoder->workers = NULL;
                goto cleanup_output_queue;
            }
        }
    }
    
    // Create copy thread
    decoder->copy_thread = (Av1CopyThread *)av1_mem_calloc(1, sizeof(Av1CopyThread));
    if (!decoder->copy_thread) {
        fprintf(stderr, "av1_create_decoder: failed to allocate copy thread\n");
        goto cleanup_workers;
    }
    
    decoder->copy_thread->config = config->copy_thread_config;
    decoder->copy_thread->input_queue = &decoder->ready_queue;
    decoder->copy_thread->output_queue = &decoder->output_queue;
    decoder->copy_thread->running = false;
    
    if (pthread_create(&decoder->copy_thread->thread, NULL, 
                       copy_thread_func, decoder->copy_thread) != 0) {
        fprintf(stderr, "av1_create_decoder: failed to create copy thread\n");
        av1_mem_free(decoder->copy_thread);
        decoder->copy_thread = NULL;
        goto cleanup_workers;
    }
    
    // Optionally create GPU thread
    if (config->use_gpu) {
        decoder->gpu_thread = (Av1GPUThread *)av1_mem_calloc(1, sizeof(Av1GPUThread));
        if (!decoder->gpu_thread) {
            fprintf(stderr, "av1_create_decoder: failed to allocate GPU thread\n");
            // Not fatal - continue without GPU thread
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

cleanup_workers:
    if (decoder->workers) {
        for (int i = 0; i < decoder->num_workers; i++) {
            if (decoder->workers[i].running) {
                pthread_cancel(decoder->workers[i].thread);
            }
            pthread_join(decoder->workers[i].thread, NULL);
        }
        av1_mem_free(decoder->workers);
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
    
    // Stop and join copy thread
    if (decoder->copy_thread) {
        if (decoder->copy_thread->running) {
            pthread_cancel(decoder->copy_thread->thread);
        }
        pthread_join(decoder->copy_thread->thread, NULL);
        av1_mem_free(decoder->copy_thread);
        decoder->copy_thread = NULL;
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
    // Step 1: Validate decoder and state
    if (!decoder) {
        fprintf(stderr, "av1_decode: NULL decoder\n");
        return AV1_INVALID_PARAM;
    }
    
    if (!data || data_size == 0) {
        fprintf(stderr, "av1_decode: NULL data or zero size\n");
        return AV1_INVALID_PARAM;
    }
    
    // Check decoder state - must be CREATED, READY, or DECODING
    if (decoder->state != AV1_DECODER_STATE_CREATED &&
        decoder->state != AV1_DECODER_STATE_READY &&
        decoder->state != AV1_DECODER_STATE_DECODING) {
        fprintf(stderr, "av1_decode: invalid decoder state %d\n", decoder->state);
        return AV1_INVALID_PARAM;
    }
    
    // Step 2: Set state to DECODING
    decoder->state = AV1_DECODER_STATE_DECODING;
    
    // Step 3: Check if ready queue is full
    if (av1_frame_queue_is_full(&decoder->ready_queue)) {
        fprintf(stderr, "av1_decode: ready queue is full\n");
        decoder->state = AV1_DECODER_STATE_READY;
        return AV1_QUEUE_FULL;
    }
    
    // Step 4: Call AOM decode path (synchronous)
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
    
    // Step 5: Process decoded frame(s)
    int frame_ready = 0;
    int show_existing = 0;
    int dpb_slot = -1;
    uint32_t frame_id = 0;
    int show_frame = 0;
    
    // Initialize iterator to NULL before first use
    aom_codec_iter_t iter = NULL;
    
    // Get the decoded frame from AOM
    aom_image_t *img = aom_codec_get_frame(&decoder->aom_decoder, &iter);
    
    if (img) {
        // A frame was decoded - get frame flags from AOM to determine
        // if this frame should be displayed
        int frame_flags = 0;
        aom_err = aom_codec_control(&decoder->aom_decoder, AOMD_GET_FRAME_FLAGS, &frame_flags);
        
        if (aom_err == AOM_CODEC_OK) {
            // Check if this is a show_existing_frame
            show_existing = (frame_flags & AOM_FRAME_FLAG_SHOW_EXISTING) ? 1 : 0;
            
            // Check if this frame should be shown (show_frame flag)
            // In AV1, show_frame=1 means the frame should be displayed
            // AOM_FRAME_FLAG_SHOW indicates the frame should be displayed
            show_frame = (frame_flags & AOM_FRAME_FLAG_SHOW) ? 1 : 0;
        } else {
            // If we can't get frame flags, assume show_frame=1 for compatibility
            // This is a fallback - in production, proper flag handling is critical
            show_frame = 1;
        }
        
        // Only mark frame as ready if it should be displayed
        // Reference-only frames (show_frame=0) should not be marked as ready
        frame_ready = show_frame;
        
        // Assign frame ID
        frame_id = decoder->next_frame_id++;
        
        // For now, use a simple slot assignment
        // In a full implementation, we'd get the actual DPB slot from AOM
        dpb_slot = frame_id % 16;  // Simple round-robin
        
        // Only push to ready queue if frame should be displayed
        if (frame_ready) {
            Av1FrameEntry entry = {
                .frame_id = frame_id,
                .dpb_slot = dpb_slot,
                .show_frame = show_frame,
                .show_existing_frame = show_existing
            };
            
            if (av1_frame_queue_push(&decoder->ready_queue, &entry) != 0) {
                fprintf(stderr, "av1_decode: failed to push to ready queue\n");
                decoder->state = AV1_DECODER_STATE_READY;
                return AV1_ERROR;
            }
        }
        
        decoder->frames_decoded++;
        
        printf("av1_decode: decoded frame_id=%u, dpb_slot=%d, show=%d, show_existing=%d, ready=%d\n",
               frame_id, dpb_slot, show_frame, show_existing, frame_ready);
    } else {
        // No frame output (e.g., frame not yet displayable)
        frame_ready = 0;
        printf("av1_decode: no frame output (not yet displayable)\n");
    }
    
    // Step 6: Fill output result
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

Av1DecodeResult av1_get_decoded_frame(Av1Decoder *decoder, 
                                       Av1FrameEntry *out_entry,
                                       uint32_t timeout_us) {
    if (!decoder || !out_entry) {
        return AV1_INVALID_PARAM;
    }
    
    // Pop from ready queue
    int result = av1_frame_queue_pop(&decoder->ready_queue, out_entry, timeout_us);
    
    if (result == 0) {
        decoder->frames_output++;
        return AV1_OK;
    }
    
    return AV1_ERROR;
}

Av1DecodeResult av1_release_frame(Av1Decoder *decoder, uint32_t frame_id) {
    (void)decoder;
    (void)frame_id;
    // In a full implementation, this would release the DPB slot
    // For now, it's a no-op
    return AV1_OK;
}
```

### test_av1_decode.c
```c
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
               entry.frame_id, entry.dpb_slot, entry.show_frame, entry.show_existing_frame);
        
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
```