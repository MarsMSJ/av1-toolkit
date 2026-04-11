

Looking at the four versions of `av1_decoder_api.h`, I need to analyze the evolution and identify any issues before creating a unified version.

## Analysis

### Version Evolution:
1. **prompt_03**: Base version - memory/query API only
2. **prompt_05**: Added decode API (Av1DecodeResult, Av1DecodeOutput, av1_decode, av1_get_decoded_frame, av1_release_frame)
3. **prompt_06**: Added output API (Av1OutputBuffer, Av1PendingOutput, av1_sync, av1_set_output, av1_receive_output, AV1_DECODER_STATE_FLUSHING)
4. **prompt_07**: Added flush API (AV1_FLUSHED, av1_flush, updated docs)

### Issues Found:

| Severity | Issue | Location |
|----------|-------|----------|
| **WARNING** | Av1FrameEntry used in av1_get_decoded_frame() but av1_job_queue.h not included | prompt_05-07 |
| **WARNING** | Av1CopyJob used in Av1PendingOutput but av1_copy_thread.h not included in prompt_05 | prompt_05 |
| **STYLE** | Inconsistent include order across versions | All |
| **STYLE** | av1_decode_end() marked as "legacy" in prompt_07 but still declared | prompt_07 |

### Unified Header:

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

// ============================================================================
// Required Dependencies
// ============================================================================

// Include stream info from memory override
#include "av1_mem_override.h"

// Include copy thread for copy job handling
#include "av1_copy_thread.h"

// Include job queue for frame entry type
#include "av1_job_queue.h"

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
 * Signal end of stream to the decoder.
 * 
 * @param decoder Decoder handle
 * @return AV1_OK on success
 * @deprecated Use av1_flush() instead
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

## Summary of Changes in Unified Version

1. **Added missing includes**: Added `av1_job_queue.h` for `Av1FrameEntry` type used in `av1_get_decoded_frame()`

2. **Complete decoder state enum**: Includes `AV1_DECODER_STATE_FLUSHING` (added in prompt_06)

3. **Complete decode result enum**: Includes `AV1_FLUSHED` (added in prompt_07)

4. **All output types**: Includes `Av1OutputBuffer` and `Av1PendingOutput` from prompt_06

5. **All API functions**:
   - Memory API: `av1_query_memory`
   - Lifecycle: `av1_create_decoder`, `av1_destroy_decoder`, `av1_flush`
   - State: `av1_get_decoder_state`, `av1_get_decoder_config`, `av1_get_mem_stats`, `av1_reset_mem_stats`
   - Decode: `av1_decode`, `av1_decode_end` (deprecated), `av1_sync`, `av1_set_output`, `av1_receive_output`
   - Legacy: `av1_get_decoded_frame`, `av1_release_frame`

6. **Marked deprecated**: `av1_decode_end` is now marked as deprecated in favor of `av1_flush`

7. **Proper documentation**: Updated `av1_destroy_decoder` with complete cleanup description from prompt_07