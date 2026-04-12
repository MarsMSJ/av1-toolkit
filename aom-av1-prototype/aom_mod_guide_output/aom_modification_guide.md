

# AOM AV1 Reference Decoder Console API Modification Guide

Based on the VP9 porting patterns and the AOM source files provided, here is the concrete modification guide.

## Progress

- [x] 1. Public API Layer — analysis complete
- [x] 2. Memory Management — analysis complete
- [x] 3. Threading Architecture — analysis complete
- [x] 4. Decode Pipeline Split — analysis complete
- [x] 5. Queue / Pipeline Management — analysis complete
- [x] 6. Reference Frame / DPB Changes — analysis complete
- [x] 7. Bitstream Parsing Changes — analysis complete
- [x] 8. Post-Processing / Filtering — analysis complete
- [x] 9. Output / Copy Path — analysis complete
- [x] 10. Error Handling & Edge Cases — analysis complete

---

## Area 1: Public API Layer

### VP9 Pattern (from reports)
The VP9 port created a wrapper struct `Av1AomDecoder` containing `aom_codec_ctx_t ctx` and state management. The public API functions map to AOM functions:
- `av1AomDecCreate()` → `aom_codec_dec_init_ver()`
- `av1AomDecDecodeAu()` → `aom_codec_decode()`
- `av1AomDecSyncAu()` → `aom_codec_get_frame()` (iterator pattern)
- `av1AomDecDestroy()` → `aom_codec_destroy()`

### AOM Equivalent
**Files involved:**
- `aom/aom_decoder.h` — Public decoder API (existing)
- `av1/av1_dx_iface.c` — Decoder interface implementation (not provided but referenced)
- `av1/decoder/decoder.c` — Core decoder (`av1_receive_compressed_data`, `av1_get_raw_frame`)

**Key AOM functions:**
- `aom_codec_dec_init_ver()` — Initialize decoder (line ~90 in aom_decoder.h)
- `aom_codec_decode()` — Submit compressed data (line ~130 in aom_decoder.h)
- `aom_codec_get_frame()` — Retrieve decoded frame (line ~155 in aom_decoder.h)
- `aom_codec_destroy()` — Destroy decoder (line ~280 in aom_codec.h)

### Modifications Required

**File**: Create new `av1/decoder/av1_console_dec.h` (public API header)

```c
// New public API header - maps console API to AOM
#ifndef AV1_CONSOLE_DECODER_H_
#define AV1_CONSOLE_DECODER_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Error codes (mapped from AOM_CODEC_*)
typedef enum Av1ConsoleDecReturn {
    AV1_CONSOLE_DEC_SUCCESS = 0,
    AV1_CONSOLE_DEC_ERR_INVALID_PARAM = -1,
    AV1_CONSOLE_DEC_ERR_INVALID_CTX = -2,
    AV1_CONSOLE_DEC_ERR_MEM_ALLOC = -3,
    AV1_CONSOLE_DEC_ERR_UNSUP_BITSTREAM = -4,
    AV1_CONSOLE_DEC_ERR_UNSUP_FEATURE = -5,
    AV1_CONSOLE_DEC_ERR_CORRUPT_FRAME = -6,
    AV1_CONSOLE_DEC_ERR_BUF_TOO_SMALL = -8,
    AV1_CONSOLE_DEC_ERR_OBU_ERROR = -25,
    AV1_CONSOLE_DEC_ERR_TILE_ERROR = -26,
} Av1ConsoleDecReturn;

// Stream descriptor (input to QUERY MEMORY)
typedef struct Av1ConsoleDecStreamDesc {
    uint32_t maxFrameWidth;
    uint32_t maxFrameHeight;
    uint32_t maxDpbSize;
    int usesAnnexB;           // 1 = Annex-B format, 0 = IVF/OBU
    uint32_t numThreads;      // Thread count for tile parallelism
} Av1ConsoleDecStreamDesc;

// Instance descriptor (from QUERY MEMORY)
typedef struct Av1ConsoleDecInstanceDesc {
    uint32_t instanceSize;    // Total memory needed
    uint32_t frameBufferSize; // Per-frame buffer size
    uint32_t numFrameBuffers; // Number of frame buffers needed
} Av1ConsoleDecInstanceDesc;

// Decoding control (passed with DECODE)
typedef struct Av1ConsoleDecAuControl {
    void *callerData;
    uint64_t pts;
    uint64_t dts;
} Av1ConsoleDecAuControl;

// Decoding result (returned from SYNC)
typedef struct Av1ConsoleDecAuResult {
    int decodeStatus;         // 0 = success, negative = error
    uint32_t decodedFrameNum;
    uint64_t pts;
    uint64_t dts;
} Av1ConsoleDecAuResult;

// Output info (returned from RECEIVE OUTPUT)
typedef struct Av1ConsoleDecOutputInfo {
    uint32_t frameWidth;
    uint32_t frameHeight;
    uint32_t bitDepth;
    uint32_t chromaFormat;    // 0 = 420, 1 = 422, 2 = 444
    uint64_t pts;
} Av1ConsoleDecOutputInfo;

// API functions
Av1ConsoleDecReturn av1ConsoleDecQueryMemory(
    const Av1ConsoleDecStreamDesc *streamDesc,
    Av1ConsoleDecInstanceDesc *instanceDesc);

Av1ConsoleDecReturn av1ConsoleDecCreate(
    const Av1ConsoleDecStreamDesc *streamDesc,
    void **ppContext);

Av1ConsoleDecReturn av1ConsoleDecDecodeAu(
    void *pContext,
    const uint8_t *data,
    uint32_t size,
    const Av1ConsoleDecAuControl *control);

Av1ConsoleDecReturn av1ConsoleDecSyncAu(
    void *pContext,
    Av1ConsoleDecAuResult *result);

Av1ConsoleDecReturn av1ConsoleDecSetOutput(
    void *pContext,
    void *buffer,
    uint32_t size);

Av1ConsoleDecReturn av1ConsoleDecReceiveOutput(
    void *pContext,
    Av1ConsoleDecOutputInfo *info);

Av1ConsoleDecReturn av1ConsoleDecFlush(void *pContext);

Av1ConsoleDecReturn av1ConsoleDecReset(void *pContext);

Av1ConsoleDecReturn av1ConsoleDecDestroy(void *pContext);

#ifdef __cplusplus
}
#endif

#endif // AV1_CONSOLE_DECODER_H_
```

**File**: Create new `av1/decoder/av1_console_dec.c` (implementation)

```c
#include "av1/decoder/av1_console_dec.h"
#include "av1/decoder/decoder.h"

#include "aom/aom_decoder.h"
#include "aom/aom_codec.h"
#include "aom/aom_image.h"

// Internal decoder wrapper (mirrors VP9 pattern)
typedef enum {
    AV1_CONSOLE_DEC_STATE_UNINITIALIZED = 0,
    AV1_CONSOLE_DEC_STATE_CREATED,
    AV1_CONSOLE_DEC_STATE_DECODING,
    AV1_CONSOLE_DEC_STATE_FLUSHING,
} Av1ConsoleDecState;

typedef struct Av1ConsoleDecoder {
    aom_codec_ctx_t ctx;
    Av1ConsoleDecState state;
    Av1ConsoleDecStreamDesc streamDesc;
    
    // Frame buffer management
    aom_get_frame_buffer_cb_fn_t fb_get_cb;
    aom_release_frame_buffer_cb_fn_t fb_release_cb;
    void *cb_priv;
    
    // Output tracking
    int has_pending_frame;
    aom_image_t *pending_image;
    uint64_t pending_pts;
    
    // Annex-B handling
    int uses_annex_b;
    uint8_t *annexb_buffer;   // Pre-allocated for start code prepend
    size_t annexb_buffer_size;
    
    // Statistics
    uint32_t frame_count;
} Av1ConsoleDecoder;

// Error mapping from AOM to console API
static Av1ConsoleDecReturn map_aom_error(aom_codec_err_t err) {
    switch (err) {
        case AOM_CODEC_OK: return AV1_CONSOLE_DEC_SUCCESS;
        case AOM_CODEC_INVALID_PARAM: return AV1_CONSOLE_DEC_ERR_INVALID_PARAM;
        case AOM_CODEC_MEM_ERROR: return AV1_CONSOLE_DEC_ERR_MEM_ALLOC;
        case AOM_CODEC_UNSUP_BITSTREAM: return AV1_CONSOLE_DEC_ERR_UNSUP_BITSTREAM;
        case AOM_CODEC_UNSUP_FEATURE: return AV1_CONSOLE_DEC_ERR_UNSUP_FEATURE;
        case AOM_CODEC_CORRUPT_FRAME: return AV1_CONSOLE_DEC_ERR_CORRUPT_FRAME;
        default: return AV1_CONSOLE_DEC_ERR_INVALID_PARAM;
    }
}

// QUERY MEMORY implementation
Av1ConsoleDecReturn av1ConsoleDecQueryMemory(
    const Av1ConsoleDecStreamDesc *streamDesc,
    Av1ConsoleDecInstanceDesc *instanceDesc) {
    
    if (!streamDesc || !instanceDesc) 
        return AV1_CONSOLE_DEC_ERR_INVALID_PARAM;
    
    // Estimate frame buffer size: YUV420 worst case
    // Width * Height * 1.5 bytes for 8-bit, *2 for 10-bit
    uint64_t frame_size = (uint64_t)streamDesc->maxFrameWidth * 
                          streamDesc->maxFrameHeight * 3 / 2;
    
    // AV1 requires up to 8 reference frames + current
    instanceDesc->numFrameBuffers = 8 + 1;
    instanceDesc->frameBufferSize = (uint32_t)frame_size;
    instanceDesc->instanceSize = sizeof(Av1ConsoleDecoder) + 
                                  frame_size + // annexb buffer
                                  4096; // overhead
    
    return AV1_CONSOLE_DEC_SUCCESS;
}

// CREATE DECODER implementation
Av1ConsoleDecReturn av1ConsoleDecCreate(
    const Av1ConsoleDecStreamDesc *streamDesc,
    void **ppContext) {
    
    if (!streamDesc || !ppContext)
        return AV1_CONSOLE_DEC_ERR_INVALID_PARAM;
    
    Av1ConsoleDecoder *dec = (Av1ConsoleDecoder *)calloc(1, sizeof(Av1ConsoleDecoder));
    if (!dec)
        return AV1_CONSOLE_DEC_ERR_MEM_ALLOC;
    
    // Copy stream descriptor
    dec->streamDesc = *streamDesc;
    dec->uses_annex_b = streamDesc->usesAnnexB;
    dec->state = AV1_CONSOLE_DEC_STATE_CREATED;
    
    // Allocate Annex-B buffer (max frame size + start code)
    uint64_t max_frame_size = (uint64_t)streamDesc->maxFrameWidth * 
                               streamDesc->maxFrameHeight * 3;
    dec->annexb_buffer_size = max_frame_size + 4;
    dec->annexb_buffer = (uint8_t *)malloc(dec->annexb_buffer_size);
    if (!dec->annexb_buffer) {
        free(dec);
        return AV1_CONSOLE_DEC_ERR_MEM_ALLOC;
    }
    
    // Get AV1 decoder interface
    extern aom_codec_iface_t *aom_codec_av1_dx(void);
    aom_codec_iface_t *iface = aom_codec_av1_dx();
    
    // Configure decoder
    aom_codec_dec_cfg_t cfg = {
        .threads = streamDesc->numThreads,
        .w = streamDesc->maxFrameWidth,
        .h = streamDesc->maxFrameHeight,
        .allow_lowbitdepth = 1
    };
    
    // Initialize AOM decoder
    aom_codec_err_t aom_err = aom_codec_dec_init_ver(&dec->ctx, iface, &cfg, 0,
                                                      AOM_DECODER_ABI_VERSION);
    if (aom_err != AOM_CODEC_OK) {
        free(dec->annexb_buffer);
        free(dec);
        return map_aom_error(aom_err);
    }
    
    *ppContext = dec;
    return AV1_CONSOLE_DEC_SUCCESS;
}

// DECODE implementation
Av1ConsoleDecReturn av1ConsoleDecDecodeAu(
    void *pContext,
    const uint8_t *data,
    uint32_t size,
    const Av1ConsoleDecAuControl *control) {
    
    Av1ConsoleDecoder *dec = (Av1ConsoleDecoder *)pContext;
    if (!dec || !data || size == 0)
        return AV1_CONSOLE_DEC_ERR_INVALID_PARAM;
    
    if (dec->state == AV1_CONSOLE_DEC_STATE_UNINITIALIZED)
        return AV1_CONSOLE_DEC_ERR_INVALID_CTX;
    
    const uint8_t *decode_data = data;
    size_t decode_size = size;
    
    // Handle Annex-B format: prepend start code
    if (dec->uses_annex_b) {
        if (size + 4 > dec->annexb_buffer_size)
            return AV1_CONSOLE_DEC_ERR_BUF_TOO_SMALL;
        
        dec->annexb_buffer[0] = 0x00;
        dec->annexb_buffer[1] = 0x00;
        dec->annexb_buffer[2] = 0x00;
        dec->annexb_buffer[3] = 0x01;
        memcpy(dec->annexb_buffer + 4, data, size);
        decode_data = dec->annexb_buffer;
        decode_size = size + 4;
    }
    
    dec->state = AV1_CONSOLE_DEC_STATE_DECODING;
    
    // Call AOM decode
    aom_codec_err_t aom_err = aom_codec_decode(&dec->ctx, decode_data, 
                                                decode_size, control);
    
    return map_aom_error(aom_err);
}

// SYNC implementation - get decode result
Av1ConsoleDecReturn av1ConsoleDecSyncAu(
    void *pContext,
    Av1ConsoleDecAuResult *result) {
    
    Av1ConsoleDecoder *dec = (Av1ConsoleDecoder *)pContext;
    if (!dec || !result)
        return AV1_CONSOLE_DEC_ERR_INVALID_PARAM;
    
    if (dec->state != AV1_CONSOLE_DEC_STATE_DECODING &&
        dec->state != AV1_CONSOLE_DEC_STATE_FLUSHING)
        return AV1_CONSOLE_DEC_ERR_INVALID_CTX;
    
    // Check for decoded frames via iterator
    aom_codec_iter_t iter = NULL;
    aom_image_t *img = aom_codec_get_frame(&dec->ctx, &iter);
    
    if (img) {
        // Frame decoded successfully
        result->decodeStatus = 0;
        result->decodedFrameNum = ++dec->frame_count;
        result->pts = img->pts;
        result->dts = img->pts; // AV1 uses same PTS/DTS
        
        // Store for output retrieval
        dec->has_pending_frame = 1;
        dec->pending_image = img;
        dec->pending_pts = img->pts;
    } else {
        // No frame ready
        result->decodeStatus = 0;
        result->decodedFrameNum = 0;
    }
    
    return AV1_CONSOLE_DEC_SUCCESS;
}

// SET OUTPUT - register external frame buffer
Av1ConsoleDecReturn av1ConsoleDecSetOutput(
    void *pContext,
    void *buffer,
    uint32_t size) {
    
    Av1ConsoleDecoder *dec = (Av1ConsoleDecoder *)pContext;
    if (!dec)
        return AV1_CONSOLE_DEC_ERR_INVALID_PARAM;
    
    // Use AOM's external frame buffer API
    // This requires implementing get_fb_cb and release_fb_cb
    // For now, we use internal allocation (AOM default)
    
    (void)buffer;
    (void)size;
    
    return AV1_CONSOLE_DEC_SUCCESS;
}

// RECEIVE OUTPUT - get frame data
Av1ConsoleDecReturn av1ConsoleDecReceiveOutput(
    void *pContext,
    Av1ConsoleDecOutputInfo *info) {
    
    Av1ConsoleDecoder *dec = (Av1ConsoleDecoder *)pContext;
    if (!dec || !info)
        return AV1_CONSOLE_DEC_ERR_INVALID_PARAM;
    
    if (!dec->has_pending_frame || !dec->pending_image)
        return AV1_CONSOLE_DEC_ERR_INVALID_CTX;
    
    aom_image_t *img = dec->pending_image;
    
    info->frameWidth = img->d_w;
    info->frameHeight = img->d_h;
    info->bitDepth = img->bit_depth;
    info->chromaFormat = (img->x_chroma_shift << 1) | img->y_chroma_shift;
    info->pts = dec->pending_pts;
    
    // Note: Actual pixel data is in img->planes[0/1/2] with strides img->stride[0/1/2]
    
    dec->has_pending_frame = 0;
    dec->pending_image = NULL;
    
    return AV1_CONSOLE_DEC_SUCCESS;
}

// FLUSH - drain remaining frames
Av1ConsoleDecReturn av1ConsoleDecFlush(void *pContext) {
    Av1ConsoleDecoder *dec = (Av1ConsoleDecoder *)pContext;
    if (!dec)
        return AV1_CONSOLE_DEC_ERR_INVALID_PARAM;
    
    dec->state = AV1_CONSOLE_DEC_STATE_FLUSHING;
    
    // Call get_frame repeatedly until NULL to flush
    aom_codec_iter_t iter = NULL;
    while (aom_codec_get_frame(&dec->ctx, &iter) != NULL) {
        // Drain frames
    }
    
    return AV1_CONSOLE_DEC_SUCCESS;
}

// RESET - reinitialize decoder
Av1ConsoleDecReturn av1ConsoleDecReset(void *pContext) {
    Av1ConsoleDecoder *dec = (Av1ConsoleDecoder *)pContext;
    if (!dec)
        return AV1_CONSOLE_DEC_ERR_INVALID_PARAM;
    
    // Destroy and recreate
    aom_codec_destroy(&dec->ctx);
    
    extern aom_codec_iface_t *aom_codec_av1_dx(void);
    aom_codec_iface_t *iface = aom_codec_av1_dx();
    
    aom_codec_dec_cfg_t cfg = {
        .threads = dec->streamDesc.numThreads,
        .w = dec->streamDesc.maxFrameWidth,
        .h = dec->streamDesc.maxFrameHeight,
        .allow_lowbitdepth = 1
    };
    
    aom_codec_err_t aom_err = aom_codec_dec_init_ver(&dec->ctx, iface, &cfg, 0,
                                                      AOM_DECODER_ABI_VERSION);
    if (aom_err != AOM_CODEC_OK)
        return map_aom_error(aom_err);
    
    dec->state = AV1_CONSOLE_DEC_STATE_CREATED;
    dec->frame_count = 0;
    dec->has_pending_frame = 0;
    
    return AV1_CONSOLE_DEC_SUCCESS;
}

// DESTROY - cleanup
Av1ConsoleDecReturn av1ConsoleDecDestroy(void *pContext) {
    Av1ConsoleDecoder *dec = (Av1ConsoleDecoder *)pContext;
    if (!dec)
        return AV1_CONSOLE_DEC_ERR_INVALID_PARAM;
    
    aom_codec_destroy(&dec->ctx);
    
    if (dec->annexb_buffer)
        free(dec->annexb_buffer);
    
    free(dec);
    
    return AV1_CONSOLE_DEC_SUCCESS;
}
```

### Gotchas
- **Iterator pattern**: AOM uses `aom_codec_get_frame(ctx, &iter)` where iter starts as NULL and returns NULL when done. Must call repeatedly to drain all frames.
- **Frame lifetime**: Frames returned by `get_frame()` are valid until next `decode()` call or `destroy()`.
- **Annex-B**: AV1 bitstreams in .ivf format don't need start codes; Annex-B (.mp4 mux) does. The VP9 port handled this with a prepended 4-byte start code.
- **ABI version**: Must pass `AOM_DECODER_ABI_VERSION` to init function (line ~95 in aom_decoder.h).

---

## Area 2: Memory Management

### VP9 Pattern (from reports)
VP9 used pool allocator with `aom_malloc`/`aom_memalign`/`aom_free`. The console API added `av1AomDecQueryMemory()` to pre-calculate required memory before allocation.

### AOM Equivalent
**Files involved:**
- `aom_mem/aom_mem.h` — Memory allocation API
- `aom_mem/aom_mem.c` — Implementation (uses standard malloc)
- `av1/common/alloccommon.c` — Frame buffer allocation (`av1_alloc_state_buffers`)
- `av1/common/av1_common_int.h` — `AV1_COMMON` struct with buffer pool

**Key AOM functions:**
- `aom_memalign()` — Aligned memory allocation (aom_mem.c)
- `aom_malloc()` — Default alignment allocation
- `aom_free()` — Free allocated memory
- `av1_alloc_state_buffers()` — Allocate frame buffers for a frame (alloccommon.c)

### Modifications Required

**File**: `aom_mem/aom_mem.h` — Add function pointer overrides for pool allocation

```c
// Add after existing function declarations (around line 50)

// Memory allocation function pointer types (for pool allocator override)
typedef void *(*aom_memalign_fn_t)(size_t align, size_t size);
typedef void *(*aom_malloc_fn_t)(size_t size);
typedef void *(*aom_calloc_fn_t)(size_t num, size_t size);
typedef void (*aom_free_fn_t)(void *memblk);

// Global function pointers for custom allocators
extern aom_memalign_fn_t aom_memalign_ptr;
extern aom_malloc_fn_t aom_malloc_ptr;
extern aom_calloc_fn_t aom_calloc_ptr;
extern aom_free_fn_t aom_free_ptr;

// Function to install custom allocators
void aom_set_memory_functions(
    aom_memalign_fn_t memalign_fn,
    aom_malloc_fn_t malloc_fn,
    aom_calloc_fn_t calloc_fn,
    aom_free_fn_t free_fn);
```

**File**: `aom_mem/aom_mem.c` — Implement function pointer system

```c
// Add at file scope (after includes, before functions)
static aom_memalign_fn_t g_memalign = aom_memalign;
static aom_malloc_fn_t g_malloc = aom_malloc;
static aom_calloc_fn_t g_calloc = aom_calloc;
static aom_free_fn_t g_free = aom_free;

// Expose pointers
aom_memalign_fn_t aom_memalign_ptr = aom_memalign;
aom_malloc_fn_t aom_malloc_ptr = aom_malloc;
aom_calloc_fn_t aom_calloc_fn_t = aom_calloc;
aom_free_fn_t aom_free_ptr = aom_free;

// Implementation of setter
void aom_set_memory_functions(
    aom_memalign_fn_t memalign_fn,
    aom_malloc_fn_t malloc_fn,
    aom_calloc_fn_t calloc_fn,
    aom_free_fn_t free_fn) {
    g_memalign = memalign_fn ? memalign_fn : aom_memalign;
    g_malloc = malloc_fn ? malloc_fn : aom_malloc;
    g_calloc = calloc_fn ? calloc_fn : aom_calloc;
    g_free = free_fn ? free_fn : aom_free;
    
    // Update exposed pointers
    aom_memalign_ptr = g_memalign;
    aom_malloc_ptr = g_malloc;
    aom_calloc_ptr = g_calloc;
    aom_free_ptr = g_free;
}

// Modify existing functions to use function pointers
void *aom_memalign(size_t align, size_t size) {
    return g_memalign(align, size);  // Changed from direct malloc
}

void *aom_malloc(size_t size) {
    return g_malloc(size);
}

void *aom_calloc(size_t num, size_t size) {
    return g_calloc(num, size);
}

void aom_free(void *memblk) {
    if (memblk) g_free(memblk);
}
```

**File**: `av1/decoder/decoder.c` — Modify `av1_decoder_create()` to accept external buffer pool

```c
// Current signature (line ~95):
// struct AV1Decoder *av1_decoder_create(BufferPool *const pool);

// New signature for console API:
struct AV1Decoder *av1_decoder_create_with_pool(
    BufferPool *const pool,
    void *(*alloc_cb)(size_t size, void *ctx),
    void (*free_cb)(void *ptr, void *ctx),
    void *alloc_ctx);
```

**File**: `av1/common/av1_common_int.h` — BufferPool structure (reference)

```c
// This exists in AV1_COMMON - shows how frame buffers are managed
// Look for BufferPool definition in av1_common_int.h

typedef struct BufferPool {
    // Array of frame buffers
    RefCntBuffer *frame_buffers[MAX_NUM_REF_FRAMES + 1];
    // Lock for thread safety
    void *lock;
    // Callbacks for external allocation
    aom_get_frame_buffer_cb_fn_t get_fb_cb;
    aom_release_frame_buffer_cb_fn_t release_fb_cb;
    void *cb_priv;
} BufferPool;
```

### Gotchas
- **Frame buffer vs. decoder memory**: The console API's `QueryMemory` must account for both decoder instance memory AND frame buffers (DPB). AV1 can have up to 8 reference frames + current.
- **Alignment**: AOM uses 32-byte alignment for SIMD. Custom allocators must preserve this.
- **BufferPool lifetime**: The `BufferPool` must outlive the decoder. In console API, this is managed by the caller.
- **Reference counting**: AV1 uses `RefCntBuffer` with reference counting. Custom allocators must support this pattern.

---

## Area 3: Threading Architecture

### VP9 Pattern (from reports)
VP9 had N decoding workers + 1 copy thread + GPU thread. The console API exposed `numThreads` config to control parallelism.

### AOM Equivalent
**Files involved:**
- `aom_util/aom_thread.h` — Worker thread interface
- `av1/decoder/decoder.h` — `AV1Decoder` struct with worker arrays
- `av1/decoder/decoder.c` — Worker initialization

**Key AOM structures:**
- `AVxWorker` — Thread worker (aom_thread.h)
- `AVxWorkerInterface` — Worker vtable (aom_thread.h)
- `pbi->num_workers` — Number of tile workers (decoder.h line ~180)
- `pbi->lf_worker` — Loop filter worker
- `pbi->tile_workers` — Array of tile decode workers

### Modifications Required

**File**: `av1/decoder/decoder.h` — Add console API thread control

```c
// Add to AV1Decoder struct (around line 180):
typedef struct AV1Decoder {
    // ... existing fields ...
    
    // Console API thread control
    int console_num_threads;        // Requested thread count
    int console_row_mt;             // Row-multithreading enabled
    
    // ... rest of existing fields ...
} AV1Decoder;
```

**File**: `av1/decoder/decoder.c` — Modify `av1_decoder_create()` to respect thread count

```c
// In av1_decoder_create(), after pool initialization:
// Current: pbi->max_threads = 0; (uses default)
//
// Add console API parameter:
struct AV1Decoder *av1_decoder_create_console(
    BufferPool *const pool,
    int num_threads,
    int row_mt) {
    
    AV1Decoder *pbi = // ... existing allocation code ...
    
    // Console API thread configuration
    pbi->console_num_threads = num_threads;
    pbi->console_row_mt = row_mt;
    
    // Map to AOM internal threading
    // num_threads controls both tile parallelism and row-mt
    if (num_threads > 1) {
        pbi->row_mt = row_mt ? 1 : 0;
        pbi->max_threads = num_threads;
    } else {
        pbi->row_mt = 0;
        pbi->max_threads = 1;
    }
    
    // ... rest of existing initialization ...
}
```

**File**: `av1/decoder/decodeframe.c` — Worker thread setup (reference for modifications)

```c
// In decodeframe.c, the function av1_decode_tg_tiles_and_wrapup()
// handles tile worker distribution. This is where console API
// thread count takes effect.
//
// Key function to understand:
// void av1_decode_tiles(AV1Decoder *pbi, const uint8_t *data, ...)
//
// For console API, you may want to:
// 1. Limit worker count via pbi->num_workers
// 2. Disable row-mt if console requests single-threaded decode
// 3. Add synchronization points for non-blocking constraint
```

### Gotchas
- **Tile vs. row parallelism**: AV1 supports two levels of parallelism:
  - Tile parallelism: Each tile decoded by separate worker
  - Row-MT: Superblock rows decoded in parallel within a tile
- **Thread count mapping**: AOM's `threads` config in `aom_codec_dec_cfg_t` controls both. For console API, you may want explicit control.
- **Worker synchronization**: The console API's non-blocking requirement means workers must not block the caller thread. AOM currently uses `winterface->sync()` which can block.
- **Loop filter is separate**: The loop filter runs on `pbi->lf_worker` after tile decode completes. This is a synchronization point.

---

## Area 4: Decode Pipeline Split

### VP9 Pattern (from reports)
The critical split separates header parsing (on caller thread) from tile decoding (on workers). This enables non-blocking decode.

### AOM Equivalent
**Files involved:**
- `av1/decoder/obu.c` — OBU parsing and frame header decode
- `av1/decoder/decodeframe.c` — Tile decoding
- `av1/decoder/decoder.h` — `av1_receive_compressed_data()` declaration

**Key AOM functions:**
- `aom_decode_frame_from_obus()` — Top-level frame decode (obu.c)
- `av1_decode_frame_headers_and_setup()` — Parse headers, setup (obu.c)
- `av1_decode_tg_tiles_and_wrapup()` — Decode tiles, run post-filter (decodeframe.c)

### Modifications Required

**File**: `av1/decoder/obu.h` — Add split-phase API

```c
// Add to obu.h for console API split decode

// Phase 1: Parse headers only (non-blocking, stays on caller thread)
typedef struct Av1DecodeHeadersResult {
    int frame_found;           // 1 if a frame was parsed
    int frame_width;
    int frame_height;
    int is_keyframe;
    int num_tiles;
    int tile_rows;
    int tile_cols;
    // ... other header info ...
} Av1DecodeHeadersResult;

Av1DecodeHeadersResult av1_decode_frame_headers(
    AV1Decoder *pbi,
    const uint8_t *data,
    size_t size,
    size_t *bytes_consumed);

// Phase 2: Decode tiles (can be async on workers)
int av1_decode_tiles_async(
    AV1Decoder *pbi,
    const uint8_t *data,
    size_t size,
    size_t header_bytes_consumed);

// Phase 3: Get output after workers complete
int av1_decode_get_output(AV1Decoder *pbi);
```

**File**: `av1/decoder/obu.c` — Implement split-phase decode

```c
// Current flow in aom_decode_frame_from_obus():
// 1. Read temporal delimiter
// 2. Read sequence header (if needed)
// 3. Read frame header -> av1_decode_frame_headers_and_setup()
// 4. Read tile group -> av1_decode_tg_tiles_and_wrapup()
// 5. Post-filter (loop filter, cdef, restoration)
//
// For console API, split after step 3:
//
// av1_decode_frame_headers() - Phase 1
Av1DecodeHeadersResult av1_decode_frame_headers(
    AV1Decoder *pbi,
    const uint8_t *data,
    size_t size,
    size_t *bytes_consumed) {
    
    Av1DecodeHeadersResult result = {0};
    
    // Parse OBUs up to and including frame header
    // (extract from existing aom_decode_frame_from_obus logic)
    
    // Return header info without decoding tiles
    result.frame_found = 1;
    result.frame_width = pbi->common.width;
    result.frame_height = pbi->common.height;
    result.num_tiles = pbi->tile_count_minus_1 + 1;
    result.tile_rows = pbi->tile_rows;
    result.tile_cols = pbi->tile_cols;
    
    *bytes_consumed = /* bytes consumed by headers */;
    
    return result;
}

// av1_decode_tiles_async() - Phase 2
int av1_decode_tiles_async(
    AV1Decoder *pbi,
    const uint8_t *data,
    size_t size,
    size_t header_bytes_consumed) {
    
    // Continue from where headers left off
    // Call av1_decode_tg_tiles_and_wrapup() with remaining data
    // This runs on worker threads
    
    return av1_decode_tg_tiles_and_wrapup(pbi, 
        data + header_bytes_consumed, 
        size - header_bytes_consumed);
}
```

### Gotchas
- **State machine**: AV1 decoder has internal state (`pbi->need_resync`, `pbi->seen_frame_header`). Split-phase decode must preserve this.
- **Reference frame setup**: `av1_decode_frame_headers_and_setup()` sets up reference frames. This must complete before tile decode.
- **Tile data access**: Tile data starts after frame header. The split must know exact byte offset.
- **Error handling**: If header parse fails, tiles should not be dispatched.

---

## Area 5: Queue / Pipeline Management

### VP9 Pattern (from reports)
VP9 used a job queue between caller and workers with ring buffer design, backpressure (QUEUE_FULL), and in-flight frame tracking.

### AOM Equivalent
**Files involved:**
- `av1/decoder/decoder.h` — `AV1DecTileMT` struct for tile job queue
- `av1/decoder/decoder.c` — Job allocation/deallocation

**Key AOM structures:**
- `AV1DecTileMT` — Tile job queue (decoder.h line ~130)
- `TileJobsDec` — Individual tile job (decoder.h line ~125)
- `pbi->tile_mt_info` — Multi-threaded tile info

### Modifications Required

**File**: `av1/decoder/decoder.h` — Add console API job queue

```c
// Add to AV1Decoder struct for console API

typedef struct Av1ConsoleJobQueue {
    // Ring buffer for decode jobs
    void **jobs;               // Array of job pointers
    int job_capacity;          // Max jobs (power of 2)
    int job_head;              // Next job to process
    int job_tail;              // Next free slot
    
    // Synchronization
    void *lock;                // Queue lock
    void *not_empty;           // Signal for workers
    void *not_full;            // Signal for caller
    
    // Status
    int num_pending;           // Jobs waiting to start
    int num_in_flight;         // Jobs currently processing
    int num_completed;         // Jobs finished, awaiting output
    
} Av1ConsoleJobQueue;

// Add to AV1Decoder
typedef struct AV1Decoder {
    // ... existing fields ...
    
    // Console API job queue
    Av1ConsoleJobQueue *console_job_queue;
    int console_queue_depth;   // Max pending frames
    
    // ... rest of existing fields ...
} AV1Decoder;
```

**File**: `av1/decoder/decoder.c` — Implement job queue

```c
// Job queue implementation
int av1_console_job_queue_init(Av1ConsoleJobQueue *q, int capacity) {
    q->jobs = (void **)calloc(capacity, sizeof(void *));
    if (!q->jobs) return -1;
    
    q->job_capacity = capacity;
    q->job_head = q->job_tail = 0;
    q->num_pending = q->num_in_flight = q->num_completed = 0;
    
    // Initialize synchronization primitives
    // (platform-specific: pthread_mutex_t, etc.)
    
    return 0;
}

int av1_console_job_queue_push(Av1ConsoleJobQueue *q, void *job) {
    // Check for backpressure
    if (q->num_pending + q->num_in_flight >= q->job_capacity) {
        return -1; // QUEUE_FULL
    }
    
    // Add job to queue
    q->jobs[q->job_tail] = job;
    q->job_tail = (q->job_tail + 1) & (q->job_capacity - 1);
    q->num_pending++;
    
    // Signal workers
    // (platform-specific: pthread_cond_signal)
    
    return 0;
}

void *av1_console_job_queue_pop(Av1ConsoleJobQueue *q) {
    // Wait for job (blocking)
    while (q->num_pending == 0) {
        // Wait on not_empty condition
    }
    
    void *job = q->jobs[q->job_head];
    q->job_head = (q->job_head + 1) & (q->job_capacity - 1);
    q->num_pending--;
    q->num_in_flight++;
    
    return job;
}

void av1_console_job_queue_complete(Av1ConsoleJobQueue *q, void *job) {
    q->num_in_flight--;
    q->num_completed++;
    // Signal caller that output is ready
}
```

### Gotchas
- **Backpressure handling**: If queue is full, `DecodeAu` should return `AV1_CONSOLE_DEC_ERR_QUEUE_FULL` (or block). VP9 removed this error, but console API may need it.
- **In-flight tracking**: Must track frames currently being decoded (not just queued) to know when DPB can be modified.
- **Output ordering**: AV1 outputs in presentation order (PTS), which may differ from decode order. The queue must handle this.

---

## Area 6: Reference Frame / DPB Changes

### VP9 Pattern (from reports)
VP9's DPB management needed careful reference counting with async pipeline. AV1 uses `RefCntBuffer` with `ref_count` field.

### AOM Equivalent
**Files involved:**
- `av1/common/av1_common_int.h` — `AV1_COMMON` struct with DPB
- `av1/common/av1_common_int.h` — `RefCntBuffer` struct
- `av1/decoder/decoder.c` — `update_frame_buffers()` function

**Key AOM structures:**
- `RefCntBuffer` — Reference counted frame buffer (av1_common_int.h)
- `cm->ref_frame_map[8]` — DPB slots (av1_common_int.h)
- `cm->cur_frame` — Current frame being decoded
- `pbi->output_frames[]` — Output queue (decoder.h line ~175)

### Modifications Required

**File**: `av1/common/av1_common_int.h` — Reference frame management

```c
// RefCntBuffer structure (existing, for reference):
typedef struct RefCntBuffer {
    YV12_BUFFER_CONFIG buf;
    int ref_count;
    int order_hint;
    FrameIndex frame_index;
    // ... other fields ...
} RefCntBuffer;

// Console API: Add reference tracking for async pipeline
typedef struct Av1ConsoleRefTracker {
    // Track which DPB slots are "locked" for async decode
    int locked_slots[8];       // 1 if slot is in use by worker
    int num_locked;
    
    // Pending reference releases (to apply after workers finish)
    int pending_release[8];
    int num_pending_release;
    
    // Current frame reference (held during tile decode)
    RefCntBuffer *cur_frame_ref;
    
} Av1ConsoleRefTracker;
```

**File**: `av1/decoder/decoder.c` — Modify `update_frame_buffers()` for async

```c
// Current function (line ~380):
// static void update_frame_buffers(AV1Decoder *pbi, int frame_decoded)
//
// For console API async pipeline, add:
// 1. Lock reference frames before dispatching to workers
// 2. Release locks after workers complete

void av1_console_lock_references(AV1Decoder *pbi) {
    AV1_COMMON *cm = &pbi->common;
    BufferPool *pool = cm->buffer_pool;
    
    // Lock all reference frames that will be needed
    for (int i = 0; i < INTER_REFS_PER_FRAME; i++) {
        int ref_idx = cm->remapped_ref_idx[i];
        if (ref_idx >= 0 && cm->ref_frame_map[ref_idx]) {
            RefCntBuffer *buf = cm->ref_frame_map[ref_idx];
            lock_buffer_pool(pool);
            buf->ref_count++;
            unlock_buffer_pool(pool);
        }
    }
}

void av1_console_unlock_references(AV1Decoder *pbi) {
    AV1_COMMON *cm = &pbi->common;
    BufferPool *pool = cm->buffer_pool;
    
    // Unlock reference frames after workers complete
    for (int i = 0; i < INTER_REFS_PER_FRAME; i++) {
        int ref_idx = cm->remapped_ref_idx[i];
        if (ref_idx >= 0 && cm->ref_frame_map[ref_idx]) {
            RefCntBuffer *buf = cm->ref_frame_map[ref_idx];
            lock_buffer_pool(pool);
            decrease_ref_count(buf, pool);
            unlock_buffer_pool(pool);
        }
    }
}
```

### Gotchas
- **Reference lifetime**: Reference frames must not be modified or recycled while workers are using them. The console API must hold extra references during tile decode.
- **DPB size**: AV1 supports up to 7 reference frames + current. Console API's `QueryMemory` must account for this.
- **Output vs. reference**: Frames in `pbi->output_frames[]` are for display, not reference. Don't confuse the two.
- **Film grain**: Film grain is applied at output time, not stored in DPB. This is handled separately.

---

## Area 7: Bitstream Parsing Changes

### VP9 Pattern (from reports)
AV1 uses OBU (OBject Unit) parsing instead of VP9's frame superframe structure. Key OBUs: Temporal Delimiter, Sequence Header, Frame Header, Tile Group.

### AOM Equivalent
**Files involved:**
- `av1/decoder/obu.h` — OBU parsing declarations
- `av1/decoder/obu.c` — OBU parsing implementation
- `av1/decoder/obu.h` — `aom_decode_frame_from_obus()` function

**Key AOM functions:**
- `aom_decode_frame_from_obus()` — Main OBU parsing loop (obu.c)
- `read_obu_header()` — Parse OBU header
- `read_sequence_header()` — Parse AV1 sequence header
- `av1_decode_frame_headers_and_setup()` — Parse frame header

### Modifications Required

**File**: `av1/decoder/obu.h` — Add OBU-level access for console API

```c
// Add OBU type enum for console API (mirrors aom_codec.h)
typedef enum {
    AV1_OBU_TEMPORAL_DELIMITER = 2,
    AV1_OBU_SEQUENCE_HEADER = 1,
    AV1_OBU_FRAME_HEADER = 3,
    AV1_OBU_TILE_GROUP = 4,
    AV1_OBU_METADATA = 5,
    AV1_OBU_FRAME = 6,
    AV1_OBU_TILE_LIST = 8,
} Av1OBUType;

// Console API: Parse OBU without decoding
typedef struct Av1OBUInfo {
    Av1OBUType type;
    size_t header_size;
    size_t payload_size;
    int has_size_field;        // 1 if size in header, 0 if terminated by size
} Av1OBUInfo;

Av1OBUInfo av1_parse_obu_header(const uint8_t *data, size_t size);

// Console API: Check if data starts with temporal delimiter (Annex-B sync)
int av1_is_temporal_delimiter(const uint8_t *data, size_t size);
```

**File**: `av1/decoder/obu.c` — Implement OBU parsing helpers

```c
// av1_parse_obu_header() implementation
Av1OBUInfo av1_parse_obu_header(const uint8_t *data, size_t size) {
    Av1OBUInfo info = {0};
    
    if (size < 2) return info; // Need at least OBU header
    
    // Parse OBU header (same logic as existing read_obu_header)
    int obu_has_size_field = (data[0] >> 4) & 1;
    int obu_type = (data[0] >> 3) & 0xF;
    
    info.type = (Av1OBUType)obu_type;
    info.has_size_field = obu_has_size_field;
    
    // Calculate sizes based on extension bytes
    int extension_bytes = (data[0] & 0x3) + 1;
    info.header_size = 1 + extension_bytes;
    
    if (obu_has_size_field && size > info.header_size) {
        // Read size from payload
        info.payload_size = read_uvlc(data + info.header_size);
    }
    
    return info;
}

// av1_is_temporal_delimiter() - for Annex-B resync
int av1_is_temporal_delimiter(const uint8_t *data, size_t size) {
    if (size < 1) return 0;
    
    // Temporal delimiter is OBU type 2 with no payload
    return (data[0] >> 3) == 2;
}
```

### Gotchas
- **OBU vs. frame**: AV1 bitstream is a stream of OBUs, not frames. A "frame" OBU contains the actual compressed frame data.
- **Sequence header persistence**: Once read, sequence header applies to all subsequent frames until a new one appears.
- **Annex-B resync**: In Annex-B format, a temporal delimiter OBU signals frame boundary. The console API may need to scan for this on decode errors.
- **Tile group**: A frame can have multiple tile groups. The console API's `DecodeAu` should handle one Access Unit (one or more tile groups).

---

## Area 8: Post-Processing / Filtering

### VP9 Pattern (from reports)
VP9 had loop filter (deblock), CDEF, and restoration. Console API moved some to GPU. In AOM, these run on CPU workers after tile decode.

### AOM Equivalent
**Files involved:**
- `av1/common/av1_loopfilter.h` — Loop filter
- `av1/common/cdef.h` — CDEF (Constrained Directional Enhancement Filter)
- `av1/common/restoration.h` — Loop restoration
- `av1/decoder/decodeframe.c` — Post-filter chain

**Key AOM functions:**
- `av1_loop_filter_frame()` — Deblock filter (decodeframe.c)
- `av1_cdef_frame()` — CDEF filter (decodeframe.c)
- `av1_loop_restoration_filter_frame()` — Restoration (decodeframe.c)
- `av1_decode_tg_tiles_and_wrapup()` — Calls post-filters at end (decodeframe.c)

### Modifications Required

**File**: `av1/decoder/decoder.h` — Add console API post-filter control

```c
// Add to AV1Decoder for console API control
typedef struct AV1Decoder {
    // ... existing fields ...
    
    // Console API post-processing control
    int console_skip_loop_filter;    // Skip deblock
    int console_skip_cdef;           // Skip CDEF
    int console_skip_restoration;    // Skip restoration
    int console_apply_film_grain;    // Apply film grain at output
    
    // Post-filter worker synchronization
    void *pf_done_event;             // Signal when post-filter complete
    
    // ... rest of existing fields ...
} AV1Decoder;
```

**File**: `av1/decoder/decodeframe.c` — Add console API bypass

```c
// In av1_decode_tg_tiles_and_wrapup(), the post-filter chain is:
// 1. av1_loop_filter_frame() - deblock
// 2. av1_cdef_frame() - CDEF  
// 3. av1_loop_restoration_filter_frame() - restoration
//
// For console API, add bypass controls:

void av1_decode_tg_tiles_and_wrapup(AV1Decoder *pbi, ...) {
    // ... existing tile decode ...
    
    // Post-filters (add console API bypass)
    if (!pbi->console_skip_loop_filter) {
        av1_loop_filter_frame(cm, &pbi->lf_row_sync, 0, 0);
    }
    
    if (!pbi->console_skip_cdef) {
        av1_cdef_frame(cm, pbi->cdef_worker, pbi->cdef_sync, 0);
    }
    
    if (!pbi->console_skip_restoration) {
        av1_loop_restoration_filter_frame(cm, &pbi->lr_row_sync, 0);
    }
    
    // Film grain is applied at output, not here
}
```

### Gotchas
- **Post-filter is synchronous**: Currently runs after tile decode completes, on same thread. For console API, you may want to run on separate worker.
- **Film grain timing**: Film grain is NOT stored in DPB. It's applied when frame is output (in `av1_get_raw_frame()` or output copy).
- **Skip options**: Console API may want to skip post-processing for faster decode, letting GPU handle it later.
- **CDEF/restoration can be disabled**: Some profiles don't support these. Check `cm->cdef_enabled`, `cm->restoration_type`.

---

## Area 9: Output / Copy Path

### VP9 Pattern (from reports)
Console API had SET OUTPUT to register external buffer, then RECEIVE OUTPUT to get frame data. Copy thread handled plane-by-plane copy with format conversion.

### AOM Equivalent
**Files involved:**
- `av1/decoder/decoder.c` — `av1_get_raw_frame()`, `av1_get_frame_to_show()`
- `aom/aom_image.h` — `aom_image_t` structure
- `aom_scale/yv12config.h` — `YV12_BUFFER_CONFIG`

**Key AOM structures:**
- `aom_image_t` — Public image structure (aom_image.h)
- `YV12_BUFFER_CONFIG` — Internal frame buffer (yv12config.h)
- `pbi->output_frames[]` — Output queue (decoder.h line ~175)

### Modifications Required

**File**: `av1/decoder/decoder.c` — Add console API output functions

```c
// Console API: Get output frame info without copying
Av1ConsoleDecReturn av1_console_get_output_info(
    AV1Decoder *pbi,
    Av1ConsoleDecOutputInfo *info) {
    
    if (pbi->num_output_frames == 0)
        return AV1_CONSOLE_DEC_ERR_INVALID_CTX;
    
    RefCntBuffer *buf = pbi->output_frames[pbi->num_output_frames - 1];
    YV12_BUFFER_CONFIG *sd = &buf->buf;
    
    info->frameWidth = sd->y_width;
    info->frameHeight = sd->y_height;
    info->bitDepth = (sd->flags & YV12_FLAG_HIGHBITDEPTH) ? 10 : 8;
    info->chromaFormat = 0; // 420
    info->pts = buf->buf.pts;
    
    return AV1_CONSOLE_DEC_SUCCESS;
}

// Console API: Copy frame to user buffer
Av1ConsoleDecReturn av1_console_copy_output(
    AV1Decoder *pbi,
    void *user_buffer,
    uint32_t buffer_size,
    uint32_t *bytes_written) {
    
    if (pbi->num_output_frames == 0)
        return AV1_CONSOLE_DEC_ERR_INVALID_CTX;
    
    RefCntBuffer *buf = pbi->output_frames[pbi->num_output_frames - 1];
    YV12_BUFFER_CONFIG *sd = &buf->buf;
    
    // Calculate required size
    uint32_t y_size = sd->y_width * sd->y_height;
    uint32_t uv_size = sd->uv_width * sd->uv_height;
    uint32_t required = y_size * 2 + uv_size * 2; // YUV420
    
    if (buffer_size < required)
        return AV1_CONSOLE_DEC_ERR_BUF_TOO_SMALL;
    
    // Copy Y plane
    uint8_t *dst = (uint8_t *)user_buffer;
    for (int row = 0; row < sd->y_height; row++) {
        memcpy(dst, sd->y_buffer + row * sd->y_stride, sd->y_width);
        dst += sd->y_width;
    }
    
    // Copy U plane
    for (int row = 0; row < sd->uv_height; row++) {
        memcpy(dst, sd->u_buffer + row * sd->uv_stride, sd->uv_width);
        dst += sd->uv_width;
    }
    
    // Copy V plane
    for (int row = 0; row < sd->uv_height; row++) {
        memcpy(dst, sd->v_buffer + row * sd->uv_stride, sd->uv_width);
        dst += sd->uv_width;
    }
    
    // Apply film grain if present and requested
    if (pbi->console_apply_film_grain && buf->film_grain_params.apply_grain) {
        // Apply film grain to user buffer
        av1_apply_film_grain(user_buffer, &buf->film_grain_params, 
                             sd->y_width, sd->y_height);
    }
    
    *bytes_written = required;
    
    // Release output frame after copy
    pbi->num_output_frames = 0;
    
    return AV1_CONSOLE_DEC_SUCCESS;
}
```

### Gotchas
- **Plane strides**: AOM uses strides (`y_stride`, `uv_stride`) which may exceed width. Copy must use stride, not width.
- **Bit depth**: 10-bit and 12-bit use 16-bit pixels. Buffer size calculation must account for this.
- **Film grain application**: Film grain parameters are in `buf->film_grain_params`. Apply after copying pixels.
- **Output release**: After `ReceiveOutput`, the frame is consumed. Must release reference (`decrease_ref_count`).

---

## Area 10: Error Handling & Edge Cases

### VP9 Pattern (from reports)
Key edge cases: resolution change (sequence header change), flush drain, destroy cleanup, error propagation from workers.

### AOM Equivalent
**Files involved:**
- `av1/decoder/decoder.c` — `av1_receive_compressed_data()` error handling
- `aom/internal/aom_codec_internal.h` — `aom_internal_error_info` struct
- `av1/decoder/obu.c` — OBU parsing errors

**Key AOM error handling:**
- `aom_set_error()` — Set error without longjmp
- `aom_internal_error()` — Set error and longjmp to error handler
- `pbi->error` — Error info in decoder (decoder.h line ~290)

### Modifications Required

**File**: `av1/decoder/decoder.c` — Add console API error handling

```c
// Console API: Get detailed error info
Av1ConsoleDecReturn av1_console_get_error(
    AV1Decoder *pbi,
    char *error_msg,
    uint32_t msg_size) {
    
    if (!pbi) return AV1_CONSOLE_DEC_ERR_INVALID_PARAM;
    
    if (pbi->error.error_code != AOM_CODEC_OK) {
        if (error_msg && msg_size > 0) {
            strncpy(error_msg, pbi->error.detail, msg_size - 1);
            error_msg[msg_size - 1] = '\0';
        }
        return map_aom_error(pbi->error.error_code);
    }
    
    return AV1_CONSOLE_DEC_SUCCESS;
}

// Console API: Handle resolution change
// Called when sequence header changes mid-stream
Av1ConsoleDecReturn av1_console_handle_resolution_change(AV1Decoder *pbi) {
    AV1_COMMON *cm = &pbi->common;
    
    // Free old buffers
    av1_free_state_buffers(cm);
    av1_free_context_buffers(cm);
    
    // Reallocate with new dimensions
    int ret = av1_alloc_state_buffers(cm, cm->width, cm->height);
    if (ret != 0) {
        return AV1_CONSOLE_DEC_ERR_MEM_ALLOC;
    }
    
    ret = av1_alloc_context_buffers(cm, cm->width, cm->height, 
                                     cm->mi_params.mi_alloc_bsize);
    if (ret != 0) {
        return AV1_CONSOLE_DEC_ERR_MEM_ALLOC;
    }
    
    // Reset decoder state for new resolution
    pbi->need_resync = 1;
    pbi->seen_frame_header = 0;
    
    return AV1_CONSOLE_DEC_SUCCESS;
}

// Console API: Safe destroy during decode
Av1ConsoleDecReturn av1_console_destroy_safe(AV1Decoder *pbi) {
    if (!pbi) return AV1_CONSOLE_DEC_ERR_INVALID_PARAM;
    
    // If workers are running, wait for them
    const AVxWorkerInterface *winterface = aom_get_worker_interface();
    winterface->sync(&pbi->lf_worker);
    
    for (int i = 0; i < pbi->num_workers; i++) {
        winterface->sync(&pbi->tile_workers[i]);
    }
    
    // Now safe to destroy
    av1_decoder_remove(pbi);
    
    return AV1_CONSOLE_DEC_SUCCESS;
}
```

**File**: `av1/decoder/obu.c` — Error propagation from workers

```c
// In aom_decode_frame_from_obus(), errors from tile decode
// are propagated through pbi->error. For console API,
// we need to capture and return these:
//
// In av1_decode_tg_tiles_and_wrapup() error path:
//   pbi->error.error_code = AOM_CODEC_CORRUPT_FRAME;
//   pbi->error.has_detail = 1;
//   strcpy(pbi->error.detail, "Tile decode error");
//
// Console API caller should check:
//   Av1ConsoleDecReturn err = av1_console_get_error(pbi, ...);
//   if (err != AV1_CONSOLE_DEC_SUCCESS) { /* handle */ }
```

### Gotchas
- **Resolution change**: AV1 allows resolution change mid-stream via new sequence header. Console API must handle reallocation.
- **need_resync**: After error, decoder waits for keyframe/intra-only. `pbi->need_resync` tracks this.
- **Worker errors**: If worker thread hits error, it sets `pbi->error` and calls longjmp. Console API must check error after sync.
- **Flush behavior**: `Flush` should call `get_frame()` until NULL, then reset state to CREATED.
- **Memory leaks**: Destroy must wait for workers and free all allocations (see `av1_decoder_remove()` in decoder.c).

---

## Summary: Build Integration

### CMakeLists.txt Integration

```cmake
# Add to existing build
add_library(av1_console_dec STATIC
  av1/decoder/av1_console_dec.c
)

target_include_directories(av1_console_dec PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${AOM_INCLUDE_DIRS}
)

target_link_libraries(av1_console_dec PRIVATE
  aom
  pthread  # For thread primitives
)

if(BUILD_SHARED_LIBS)
  target_compile_definitions(av1_console_dec PUBLIC AV1_CONSOLE_DEC_EXPORTS)
endif()
```

### Test Vectors

Use official AV1 test vectors from aomedia.org to verify:
- Main profile (8-bit, 4:2:0)
- High profile (10-bit, 4:2:0)
- Professional profile (10/12-bit, 4:2:2)
- Various resolutions (480p, 720p, 1080p, 4K, 8K)
- Annex-B vs IVF containers
- Multiple tile configurations