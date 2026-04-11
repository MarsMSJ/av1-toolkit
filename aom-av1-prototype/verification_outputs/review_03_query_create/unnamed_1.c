#define AV1_DECODER_API_C
#include "av1_decoder_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdalign.h>
#include <sched.h>

// Include the memory override and job queue
#include "av1_mem_override.h"
#include "av1_job_queue.h"

// ============================================================================
// Constants
// ============================================================================

#define DEFAULT_ALIGNMENT      64
#define MC_BORDER              32           // Motion compensation border (reduced from 128)
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

// Forward declare AOM structures (we'll use stubs since we're not linking AOM)
struct AV1Decoder;
struct AV1Common;
struct BufferPool;

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
    
    // AOM decoder structures (stubbed for this implementation)
    void *aom_decoder;         // Opaque AOM decoder
    void *aom_common;          // Opaque AV1_COMMON
    void *buffer_pool;         // Opaque BufferPool
    
    // Frame queues
    Av1FrameQueue ready_queue;         // Decoded frames ready for output
    Av1FrameQueue output_queue;        // Frames pending output pickup
    
    // Queue storage - separate allocations for each queue
    Av1FrameEntry *ready_queue_storage;
    Av1FrameEntry *output_queue_storage;
    int queue_storage_size;
    
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
    
    // Bytes per pixel - bit_depth divided by 8, minimum 1 byte
    int bytes_per_pixel = (bit_depth + 7) / 8;
    
    // Chroma factor
    double chroma_factor;
    switch (chroma_subsampling) {
        case 2: chroma_factor = 3.0; break;   // 444
        case 1: chroma_factor = 2.0; break;   // 422
        default: chroma_factor = 1.5; break;  // 420
    }
    
    // Total size = Y plane + chroma planes
    // Y plane: padded_width * padded_height * bytes_per_pixel
    // Chroma: Y * chroma_factor - Y (since chroma is included in factor)
    size_t y_size = (size_t)padded_width * padded_height * bytes_per_pixel;
    size_t chroma_size = (size_t)(padded_width * padded_height * bytes_per_pixel * (chroma_factor - 1.0));
    size_t size = y_size + chroma_size;
    
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
    
    // Determine bit depth: is_16bit = 12-bit, otherwise use max_bitrate (8 or 10)
    int bit_depth = info->is_16bit ? 12 : (info->max_bitrate >= 10 ? 10 : 8);
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
    // NOTE: We need TWO queue storages now (ready_queue + output_queue)
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
    
    // Query memory requirements - use defaults if stream info not provided in config
    // Note: In a real implementation, config would contain stream info
    Av1StreamInfo info = {
        .width = 1920,   // Default to 1080p
        .height = 1080,
        .max_bitrate = 10,  // Default to 10-bit
        .chroma_subsampling = 0,  // 420
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
    
    // Initialize mutex and condition variable
    if (pthread_mutex_init(&decoder->decoder_mutex, NULL) != 0) {
        fprintf(stderr, "av1_create_decoder: failed to init mutex\n");
        goto cleanup_decoder;
    }
    
    if (pthread_cond_init(&decoder->decoder_cond, NULL) != 0) {
        fprintf(stderr, "av1_create_decoder: failed to init cond\n");
        pthread_mutex_destroy(&decoder->decoder_mutex);
        goto cleanup_decoder;
    }
    
    // Allocate queue storage - TWO separate queues now
    int queue_capacity = config->queue_depth + 4;  // Extra for safety
    decoder->queue_storage_size = queue_capacity * sizeof(Av1FrameEntry);
    
    // Allocate ready queue storage
    decoder->ready_queue_storage = (Av1FrameEntry *)av1_mem_memalign(DEFAULT_ALIGNMENT, 
                                                                      decoder->queue_storage_size);
    if (!decoder->ready_queue_storage) {
        fprintf(stderr, "av1_create_decoder: failed to allocate ready queue storage\n");
        goto cleanup_queues;
    }
    
    // Allocate output queue storage
    decoder->output_queue_storage = (Av1FrameEntry *)av1_mem_memalign(DEFAULT_ALIGNMENT, 
                                                                       decoder->queue_storage_size);
    if (!decoder->output_queue_storage) {
        fprintf(stderr, "av1_create_decoder: failed to allocate output queue storage\n");
        goto cleanup_ready_queue_storage;
    }
    
    // Initialize ready queue
    if (av1_frame_queue_init(&decoder->ready_queue, decoder->ready_queue_storage, 
                              queue_capacity) != 0) {
        fprintf(stderr, "av1_create_decoder: failed to init ready queue\n");
        goto cleanup_output_queue_storage;
    }
    
    // Initialize output queue
    if (av1_frame_queue_init(&decoder->output_queue, decoder->output_queue_storage, 
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
    
    // Initialize AOM decoder structures (stubbed)
    // In a real implementation, this would call aom_create_decoder()
    decoder->aom_decoder = NULL;  // Stub
    decoder->aom_common = NULL;   // Stub
    decoder->buffer_pool = NULL;  // Stub
    
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

cleanup_output_queue_storage:
    if (decoder->output_queue_storage) {
        av1_mem_free(decoder->output_queue_storage);
    }

cleanup_ready_queue_storage:
    if (decoder->ready_queue_storage) {
        av1_mem_free(decoder->ready_queue_storage);
    }

cleanup_queues:
    pthread_cond_destroy(&decoder->decoder_cond);
    pthread_mutex_destroy(&decoder->decoder_mutex);

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
    if (decoder->output_queue_storage) {
        av1_mem_free(decoder->output_queue_storage);
    }
    if (decoder->ready_queue_storage) {
        av1_mem_free(decoder->ready_queue_storage);
    }
    
    // Destroy synchronization primitives
    pthread_cond_destroy(&decoder->decoder_cond);
    pthread_mutex_destroy(&decoder->decoder_mutex);
    
    // Free AOM structures (stubbed)
    // In real implementation: aom_decoder_remove(decoder->aom_decoder);
    
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
