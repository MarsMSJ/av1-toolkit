// [lines 1–376 of 1292]
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
    
    // Calculate luma size (full padded dimensions)
    size_t luma_size = (size_t)padded_width * padded_height * bytes_per_pixel;
    
    // Calculate chroma sizes with proper subsampling
    size_t chroma_size = 0;
    switch (chroma_subsampling) {
        case 2: // 444 - chroma same dimensions as luma
            chroma_size = luma_size * 2;  // U + V planes
            break;
        case 1: // 422 - chroma half width, full height
        {
            int chroma_width = padded_width / 2;
            size_t chroma_plane_size = (size_t)chroma_width * padded_height * bytes_per_pixel;
            chroma_size = chroma_plane_size * 2;  // U + V planes
            break;
        }
        default: // 420 - chroma half width, half height
        {
            int chroma_width = padded_width / 2;
            int chroma_height = padded_height / 2;
            size_t chroma_plane_size = (size_t)chroma_width * chroma_height * bytes_per_pixel;
            chroma_size = chroma_plane_size * 2;  // U + V planes
            break;
        }
    }
    
    // Total size
    size_t size = luma_size + chroma_size;
    
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

