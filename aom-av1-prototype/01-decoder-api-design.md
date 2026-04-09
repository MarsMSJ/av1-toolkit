# AV1 Decoder API Design — Console-Style Fixed-Memory Interface

## Background: How Software Decoder APIs Work

Most production decoder APIs (hardware decoders, game consoles, FFmpeg's `AVCodec` internal model, Android `MediaCodec`, NVDEC) follow the same fundamental pattern: **the caller owns all memory, the decoder is a state machine that processes Access Units (AUs) asynchronously, and output is explicitly pulled by the application**.

This differs from the AOM reference decoder which:
- Allocates memory internally via `aom_malloc`/`aom_memalign`
- Is fully synchronous — `aom_codec_decode()` blocks until the entire frame (parse + reconstruct + filter) is complete
- Returns frames via a simple iterator (`aom_codec_get_frame()`)

### Why the Console Pattern Exists

1. **Deterministic memory** — Games/consoles have fixed memory budgets. The decoder cannot surprise the system with allocations mid-stream.
2. **Pipeline parallelism** — DECODE must return fast so the application can feed the next AU while reconstruction runs on worker threads and previous frames are being copied out.
3. **Zero-copy where possible** — SET OUTPUT / RECEIVE OUTPUT lets the app control when and where pixel data lands (e.g., GPU-visible memory, display scanout buffers).
4. **Backpressure** — QUEUE FULL tells the application "slow down, I'm still chewing on earlier work" rather than silently buffering.

### How FFmpeg Maps to This

FFmpeg's `avcodec_send_packet()` / `avcodec_receive_frame()` is the same split:

| Console API | FFmpeg Equivalent | Role |
|---|---|---|
| `QUERY MEMORY` | `avcodec_alloc_context3()` + config | Determine resources needed |
| `CREATE DECODER` | `avcodec_open2()` | Initialize with allocated resources |
| `DECODE` | `avcodec_send_packet()` | Submit compressed data (non-blocking intent) |
| `SYNC` | (implicit in receive, or `AVHWFramesContext` sync) | Signal decode complete |
| `SET OUTPUT` | `av_frame_alloc()` + `av_hwframe_transfer_data()` | Provide output destination |
| `RECEIVE OUTPUT` | `avcodec_receive_frame()` | Block/poll for decoded frame |
| `FLUSH` | `avcodec_flush_buffers()` | Drain pipeline, seek reset |

The key insight: **DECODE and OUTPUT are decoupled**. The decoder internally maintains a pipeline where multiple AUs can be in-flight at different stages.

---

## Proposed API

### Data Types

```c
typedef struct Av1MemoryRequirements {
    size_t total_size;            /* Total bytes needed */
    size_t alignment;             /* Required alignment (e.g., 64 bytes) */
    /* Breakdown (informational) */
    size_t dpb_size;              /* DPB frame buffers */
    size_t work_buffer_size;      /* Scratch/worker buffers */
    size_t context_size;          /* Decoder context + tables */
    size_t output_queue_size;     /* Output staging buffers */
} Av1MemoryRequirements;

typedef struct Av1StreamInfo {
    int max_width;
    int max_height;
    int bit_depth;                /* 8, 10, or 12 */
    int monochrome;               /* 1 = no chroma planes */
    int chroma_subsampling_x;     /* 1 for 4:2:0/4:2:2 */
    int chroma_subsampling_y;     /* 1 for 4:2:0 */
    int max_temporal_layers;
    int max_spatial_layers;
} Av1StreamInfo;

typedef struct Av1DecoderConfig {
    /* Memory */
    void  *memory_base;           /* Caller-allocated block */
    size_t memory_size;           /* Size of that block */

    /* Pipeline */
    int queue_depth;              /* Max in-flight AUs (e.g., 8) */

    /* Threading */
    int num_worker_threads;       /* Entropy + recon workers */
    int worker_thread_affinity;   /* CPU affinity mask for workers */
    int worker_thread_priority;   /* OS thread priority for workers */
    int copy_thread_priority;     /* OS thread priority for copy thread */

    /* GPU (stretch goal) */
    int use_gpu;                  /* 0 = CPU-only, 1 = GPU-assisted */
    void *gpu_device;             /* Opaque GPU device handle */
} Av1DecoderConfig;

typedef struct Av1DecodeResult {
    int      frame_ready;         /* 1 if a frame can be received */
    uint32_t frame_id;            /* Opaque ID for SET OUTPUT */
    int      show_existing_frame; /* 1 if no new decode was needed */
} Av1DecodeResult;

typedef struct Av1OutputBuffer {
    void  *planes[3];             /* Y, U, V destination pointers */
    int    strides[3];            /* Byte stride per plane */
} Av1OutputBuffer;

typedef enum Av1Status {
    AV1_OK                = 0,
    AV1_QUEUE_FULL        = 1,   /* Decode queue is full */
    AV1_NEED_MORE_DATA    = 2,   /* No frame available yet */
    AV1_END_OF_STREAM     = 3,   /* Flush complete, no more frames */
    AV1_ERROR_INVALID_PARAM = -1,
    AV1_ERROR_CORRUPT_DATA  = -2,
    AV1_ERROR_OUT_OF_MEMORY = -3,
    AV1_ERROR_INTERNAL      = -4,
} Av1Status;

/* Opaque decoder handle */
typedef struct Av1Decoder Av1Decoder;
```

### Functions

```c
/*
 * Query how much memory is needed for the given stream characteristics.
 * Call BEFORE av1_create_decoder. The caller peeks at the bitstream
 * (e.g., parses the Sequence Header OBU) to fill Av1StreamInfo.
 */
Av1Status av1_query_memory(
    const Av1StreamInfo       *stream_info,
    int                        queue_depth,
    int                        num_worker_threads,
    Av1MemoryRequirements     *out_requirements
);

/*
 * Create and initialize the decoder from caller-owned memory.
 * All internal structures, DPB, scratch buffers, thread stacks
 * are carved from config->memory_base.
 */
Av1Status av1_create_decoder(
    const Av1DecoderConfig *config,
    Av1Decoder            **out_decoder
);

/*
 * Submit an Access Unit for decoding.
 * MUST return within 200us — performs OBU/syntax parsing only.
 * Reconstruction work is queued to worker threads.
 *
 * Returns AV1_QUEUE_FULL if queue_depth AUs are already in-flight.
 * Returns AV1_OK on success; out_result indicates if a frame became
 * ready for output as a side-effect (e.g., show_existing_frame).
 */
Av1Status av1_decode(
    Av1Decoder           *decoder,
    const uint8_t        *data,
    size_t                data_size,
    Av1DecodeResult      *out_result
);

/*
 * Poll / wait for decode completion of next in-order frame.
 * Can be non-blocking (timeout_us = 0) or blocking.
 * Returns AV1_OK + result when a frame is ready.
 * Returns AV1_NEED_MORE_DATA if nothing is ready yet.
 */
Av1Status av1_sync(
    Av1Decoder           *decoder,
    uint32_t              timeout_us,
    Av1DecodeResult      *out_result
);

/*
 * Provide a destination buffer where the decoded frame will be copied.
 * The frame is identified by frame_id from Av1DecodeResult.
 * This enqueues a copy job on the copy thread.
 */
Av1Status av1_set_output(
    Av1Decoder             *decoder,
    uint32_t                frame_id,
    const Av1OutputBuffer  *output_buffer
);

/*
 * Block until the copy for frame_id is complete.
 * After this returns AV1_OK, the output_buffer planes contain valid pixels
 * and the internal DPB slot may be released (if no longer referenced).
 */
Av1Status av1_receive_output(
    Av1Decoder *decoder,
    uint32_t    frame_id,
    uint32_t    timeout_us
);

/*
 * Flush the decoder pipeline. Signals end-of-stream.
 * After calling, repeatedly call av1_sync + av1_set_output +
 * av1_receive_output until AV1_END_OF_STREAM is returned.
 * Then the decoder can be destroyed or reset for a new stream.
 */
Av1Status av1_flush(
    Av1Decoder *decoder
);

/*
 * Destroy the decoder. After this, the caller may free memory_base.
 * Joins all threads.
 */
Av1Status av1_destroy_decoder(
    Av1Decoder *decoder
);
```

---

## Typical Application Loop

```c
// 1. Parse sequence header from bitstream to get stream params
Av1StreamInfo info = { .max_width=3840, .max_height=2160, .bit_depth=10, ... };

// 2. Query memory
Av1MemoryRequirements mem_req;
av1_query_memory(&info, /*queue_depth=*/8, /*workers=*/4, &mem_req);

// 3. Allocate and create
void *mem = aligned_alloc(mem_req.alignment, mem_req.total_size);
Av1DecoderConfig cfg = {
    .memory_base = mem, .memory_size = mem_req.total_size,
    .queue_depth = 8, .num_worker_threads = 4,
    .worker_thread_priority = NORMAL, .copy_thread_priority = HIGH,
};
Av1Decoder *dec;
av1_create_decoder(&cfg, &dec);

// 4. Decode loop
while (have_data) {
    Av1DecodeResult result;
    Av1Status s = av1_decode(dec, au_data, au_size, &result);

    if (s == AV1_QUEUE_FULL) {
        // Must drain — sync + output before submitting more
        av1_sync(dec, INFINITE, &result);
        av1_set_output(dec, result.frame_id, &my_output_buf);
        av1_receive_output(dec, result.frame_id, INFINITE);
        present(my_output_buf);
        // Retry decode
        continue;
    }

    // Check if frames are ready (non-blocking)
    while (av1_sync(dec, 0, &result) == AV1_OK) {
        av1_set_output(dec, result.frame_id, &get_free_display_buf());
        av1_receive_output(dec, result.frame_id, INFINITE);
        present(...);
    }
}

// 5. Drain
av1_flush(dec);
while (av1_sync(dec, INFINITE, &result) == AV1_OK) {
    av1_set_output(dec, result.frame_id, &buf);
    av1_receive_output(dec, result.frame_id, INFINITE);
    present(buf);
}

av1_destroy_decoder(dec);
free(mem);
```
