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
