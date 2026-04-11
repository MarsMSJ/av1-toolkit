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
 * Get the next decoded frame from the ready queue.
 * 
 * This function retrieves a frame that was previously decoded and is
 * waiting for output pickup.
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
