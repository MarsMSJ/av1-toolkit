

# AOM AV1 Reference Decoder Console API Modification Guide

Based on the VP9 porting patterns and the AOM source provided, here is a concrete modification guide for implementing a console-style API on top of libaom.

## Progress

- [x] 1. Public API Layer — done
- [ ] 2. Memory Management — not yet
- [ ] 3. Threading Architecture — not yet
- [ ] 4. Decode Pipeline Split — not yet
- [ ] 5. Queue / Pipeline Management — not yet
- [ ] 6. Reference Frame / DPB Changes — not yet
- [ ] 7. Bitstream Parsing Changes — not yet
- [ ] 8. Post-Processing / Filtering — not yet
- [ ] 9. Output / Copy Path — not yet
- [ ] 10. Error Handling & Edge Cases — not yet

---

## Area 1: Public API Layer

### VP9 Pattern (from reports)
The VP9 port created a wrapper layer with functions like `av1AomDecCreate()`, `av1AomDecDecodeAu()`, `av1AomDecSyncAu()`, `av1AomDecSetDecodeOutput()`, and `av1AomDecSyncDecodeOutput()`. These map to the underlying `aom_codec_decode()` and `aom_codec_get_frame()` iterator pattern.

### AOM Equivalent
**Files involved:**
- `aom/aom_decoder.h` — Public decoder API declarations
- `aom/aom_codec.h` — Core codec types (`aom_codec_ctx_t`, `aom_codec_iter_t`)
- `av1/av1_dx_iface.c` — AV1 decoder interface implementation
- `av1/decoder/decoder.h` — `AV1Decoder` struct
- `av1/decoder/decoder.c` — `av1_receive_compressed_data()`, `av1_get_raw_frame()`

**Key functions:**
- `aom_codec_dec_init_ver()` — Initialize decoder context
- `aom_codec_decode()` — Submit compressed data
- `aom_codec_get_frame()` — Iterator to retrieve decoded frames
- `av1_receive_compressed_data()` — AV1-specific data ingestion
- `av1_get_raw_frame()` — Get frame from output queue by index
- `av1_get_frame_to_show()` — Get highest-spatial-layer output

### Modifications Required

**File: av1/decoder/decoder.h**
Add the console API wrapper struct after the existing `AV1Decoder` definition:

```c
// Console API state machine
typedef enum Av1DecConsoleState {
  AV1_DEC_CONSOLE_STATE_UNINITIALIZED = 0,
  AV1_DEC_CONSOLE_STATE_CREATED,
  AV1_DEC_CONSOLE_STATE_DECODING,
  AV1_DEC_CONSOLE_STATE_FLUSHING,
} Av1DecConsoleState;

// Console API wrapper - sits on top of AV1Decoder
typedef struct Av1ConsoleDecoder {
  aom_codec_ctx_t aom_ctx;           // AOM codec context
  AV1Decoder *pbi;                   // Internal AV1 decoder
  Av1DecConsoleState state;
  
  // Output management
  void *pending_output_buffer;
  uint32_t pending_output_size;
  int has_pending_frame;
  
  // Configuration
  uint32_t num_threads;
  int uses_annexb;
  
  // Statistics
  uint32_t frame_count;
} Av1ConsoleDecoder;
```

**File: av1/decoder/decoder.c**
Add wrapper functions that map console API to AOM:

```c
// Create decoder instance
Av1ConsoleDecoder* av1ConsoleDecCreate(const Av1DecConfig *config) {
  Av1ConsoleDecoder *dec = aom_memalign(32, sizeof(*dec));
  if (!dec) return NULL;
  memset(dec, 0, sizeof(*dec));
  
  // Initialize AOM context
  aom_codec_dec_cfg_t cfg = {
    .threads = config->num_threads,
    .w = config->max_width,
    .h = config->max_height,
    .allow_lowbitdepth = 1
  };
  
  // Get AV1 decoder interface
  extern aom_codec_iface_t *aom_codec_av1_dx(void);
  aom_codec_err_t err = aom_codec_dec_init(&dec->aom_ctx, 
                                            aom_codec_av1_dx(), 
                                            &cfg, 0);
  if (err != AOM_CODEC_OK) {
    aom_free(dec);
    return NULL;
  }
  
  // Get internal AV1Decoder from priv data
  dec->pbi = (AV1Decoder *)dec->aom_ctx.priv;
  dec->state = AV1_DEC_CONSOLE_STATE_CREATED;
  dec->num_threads = config->num_threads;
  dec->uses_annexb = config->uses_annexb;
  
  return dec;
}

// Decode Access Unit (maps to aom_codec_decode)
int av1ConsoleDecDecodeAu(Av1ConsoleDecoder *dec, 
                          const uint8_t *data, 
                          size_t size) {
  if (dec->state == AV1_DEC_CONSOLE_STATE_UNINITIALIZED)
    return AV1_DEC_ERR_INVALID_CTX;
  
  // Handle Annex-B: prepend start code if needed
  const uint8_t *input_data = data;
  uint8_t start_code[4] = { 0, 0, 0, 1 };
  uint8_t *annexb_buf = NULL;
  
  if (dec->uses_annexb) {
    annexb_buf = aom_malloc(size + 4);
    memcpy(annexb_buf, start_code, 4);
    memcpy(annexb_buf + 4, data, size);
    input_data = annexb_buf;
    size += 4;
  }
  
  dec->state = AV1_DEC_CONSOLE_STATE_DECODING;
  aom_codec_err_t err = aom_codec_decode(&dec->aom_ctx, input_data, size, NULL);
  
  if (annexb_buf) aom_free(annexb_buf);
  
  return av1ConsoleDecMapError(err);
}

// Sync/retrieve output frames (maps to aom_codec_get_frame)
int av1ConsoleDecSyncAu(Av1ConsoleDecoder *dec, 
                        Av1DecOutput *output) {
  if (dec->state != AV1_DEC_CONSOLE_STATE_DECODING &&
      dec->state != AV1_DEC_CONSOLE_STATE_FLUSHING)
    return AV1_DEC_ERR_INVALID_CTX;
  
  aom_codec_iter_t iter = NULL;
  const aom_image_t *img = aom_codec_get_frame(&dec->aom_ctx, &iter);
  
  if (!img) {
    // No frame available - this is NOT an error
    // Caller should retry or treat as "no output yet"
    return AV1_DEC_SUCCESS;  // Or a specific "no frame" indicator
  }
  
  // Copy frame data to output buffer
  output->width = img->d_w;
  output->height = img->d_h;
  output->pts = img->pts;
  // ... copy plane data
  
  dec->frame_count++;
  return AV1_DEC_SUCCESS;
}

// Set external output buffer
int av1ConsoleDecSetOutput(Av1ConsoleDecoder *dec, 
                           void *buffer, 
                           size_t size) {
  // Use AOM's external frame buffer API
  return aom_codec_set_frame_buffer_functions(
    &dec->aom_ctx, 
    dec->fb_get_cb, 
    dec->fb_release_cb, 
    dec->cb_priv);
}

// Destroy decoder
int av1ConsoleDecDestroy(Av1ConsoleDecoder *dec) {
  if (!dec) return AV1_DEC_ERR_INVALID_PARAM;
  aom_codec_destroy(&dec->aom_ctx);
  aom_free(dec);
  return AV1_DEC_SUCCESS;
}
```

### Gotchas

1. **Iterator pattern**: Unlike VP9's queue-based output, AOM uses an iterator pattern. You must call `aom_codec_get_frame()` repeatedly until it returns NULL to drain all frames from a single decode call.

2. **State machine**: The VP9 port used explicit states (`CREATED`, `DECODING`, `FLUSHING`). AOM doesn't enforce this—your wrapper must track state to prevent invalid operations (e.g., calling sync before decode).

3. **Annex-B handling**: AV1 bitstreams can be in Annex-B (with start codes) or IVF/obfuscated format. The wrapper must handle both. The VP9 report correctly notes this requires prepending 0x00000001 before the OBU data.

---

## Area 2: Memory Management

### VP9 Pattern (from reports)
The VP9 port replaced direct `aom_malloc/aom_memalign/aom_free` calls with a pool allocator. The key insight is that AOM provides hooks via `aom_mem.h` macros that can be overridden, and frame buffer allocation can be delegated to external callbacks.

### AOM Equivalent
**Files involved:**
- `aom_mem/aom_mem.h` — Memory allocation macros (`aom_malloc`, `aom_memalign`, `aom_free`)
- `aom_mem/aom_mem.c` — Default implementations
- `aom/aom_frame_buffer.h` — External frame buffer API
- `av1/common/alloccommon.c` — Frame buffer allocation (`av1_alloc_state_buffers`, `av1_free_state_buffers`)
- `av1/common/av1_common_int.h` — `BufferPool` struct

**Key structures:**
- `BufferPool` — Manages reference frame buffers
- `RefCntBuffer` — Reference-counted frame buffer
- `aom_get_frame_buffer_cb_fn_t` — External buffer get callback
- `aom_release_frame_buffer_cb_fn_t` — External buffer release callback

### Modifications Required

**File: aom/aom_frame_buffer.h**
Add console API memory query function:

```c
// Query memory requirements for a given configuration
typedef struct Av1DecMemoryReq {
  uint32_t instance_size;      // Size of decoder instance
  uint32_t frame_buffer_size;  // Per-frame buffer size
  uint32_t num_frame_buffers;  // Number of buffers needed (DPB size)
  uint32_t max_dpb_size;       // Maximum DPB size for this config
} Av1DecMemoryReq;

aom_codec_err_t av1_dec_query_memory_req(uint32_t max_width,
                                          uint32_t max_height,
                                          uint32_t num_threads,
                                          Av1DecMemoryReq *req);
```

**File: av1/decoder/decoder.c**
Implement memory query by probing AOM:

```c
aom_codec_err_t av1_dec_query_memory_req(uint32_t max_width,
                                          uint32_t max_height,
                                          uint32_t num_threads,
                                          Av1DecMemoryReq *req) {
  if (!req) return AOM_CODEC_INVALID_PARAM;
  
  // Estimate based on resolution and threading
  // AV1 max DPB is 8 frames for non-svc, more for svc
  req->max_dpb_size = 8;  // Conservative for non-svc
  
  // Frame buffer size: YUV420 8-bit
  // Y plane: width * height
  // U/V planes: width/2 * height/2 each
  uint64_t frame_size = (uint64_t)max_width * max_height * 3 / 2;
  
  // Add alignment padding
  frame_size = (frame_size + 127) & ~127;
  
  req->frame_buffer_size = (uint32_t)frame_size;
  req->num_frame_buffers = req->max_dpb_size + 1;  // +1 for current frame
  
  // Instance size: AV1Decoder + thread data
  req->instance_size = sizeof(AV1Decoder);
  if (num_threads > 1) {
    req->instance_size += sizeof(DecWorkerData) * num_threads;
  }
  
  return AOM_CODEC_OK;
}
```

**File: av1/decoder/decoder.h**
Add external buffer pool management to `AV1Decoder`:

```c
typedef struct AV1Decoder {
  // ... existing fields ...
  
  // External frame buffer support (add these)
  int use_external_fb;  // Boolean: using external frame buffers
  void *ext_fb_priv;    // Private data for external FB callbacks
  
  // ... rest of existing fields ...
} AV1Decoder;
```

**File: av1/common/alloccommon.c**
Add ability to use pre-allocated external buffers:

```c
int av1_alloc_state_buffers(struct AV1Common *cm, int width, int height) {
  // ... existing allocation logic ...
  
  // If external frame buffers are configured, skip internal allocation
  if (cm->buffer_pool->use_external) {
    // Verify external buffers are sufficient
    if (cm->buffer_pool->num_fb < cm->max_dpb_size) {
      return AOM_CODEC_MEM_ERROR;
    }
    return 0;
  }
  
  // ... existing allocation ...
}
```

### Gotchas

1. **Buffer pool lifetime**: The `BufferPool` must outlive the decoder. In console API, the caller owns the pool and passes it at create time.

2. **Reference counting**: AOM uses `RefCntBuffer` with reference counting. When using external buffers, you must implement the release callback to actually release references, not free memory.

3. **Thread safety**: The buffer pool uses mutexes for thread safety. If your console API runs decode on a different thread than output retrieval, you need proper synchronization.

4. **Resolution changes**: When resolution changes mid-stream, AOM reallocates buffers. Your external buffer allocation must handle this or return an error.

---

## Area 3: Threading Architecture

### VP9 Pattern (from reports)
The VP9 decoder had N decoding workers + 1 copy thread + (optionally) GPU thread. The console API exposed thread count as a parameter. AOM has its own multi-threading: row-level parallelism via `row_mt` and tile parallelism via multiple workers.

### AOM Equivalent
**Files involved:**
- `av1/decoder/decoder.h` — `AV1Decoder` struct with worker threads
- `av1/decoder/decoder.c` — Worker initialization in `av1_decoder_create()`
- `aom_util/aom_thread.h` — `AVxWorker` interface
- `av1/common/thread_common.h` — `AV1LfSync`, `AV1LrSync`, `AV1CdefSync`

**Key structures:**
- `AVxWorker` — Thread worker abstraction
- `AV1LfSync` — Loop filter synchronization
- `AV1DecRowMTInfo` — Row-level MT state
- `AV1DecTileMT` — Tile-level MT state

### Modifications Required

**File: av1/decoder/decoder.h**
Add console API thread configuration to `AV1Decoder`:

```c
typedef struct AV1Decoder {
  // ... existing fields ...
  
  // Console API thread configuration
  uint32_t num_worker_threads;      // Total worker threads for decode
  uint32_t num_tile_workers;        // Workers for tile parallelism
  int row_mt_enabled;               // Row-level MT enabled
  
  // ... rest of existing fields ...
} AV1Decoder;
```

**File: av1/decoder/decoder.c**
Modify `av1_decoder_create()` to accept thread configuration:

```c
AV1Decoder *av1_decoder_create_ex(BufferPool *const pool,
                                   const Av1DecThreadConfig *thread_cfg) {
  AV1Decoder *volatile const pbi = aom_memalign(32, sizeof(*pbi));
  if (!pbi) return NULL;
  
  // ... existing initialization ...
  
  // Apply thread configuration from console API
  if (thread_cfg) {
    pbi->max_threads = thread_cfg->num_threads;
    pbi->row_mt = thread_cfg->enable_row_mt;
    
    // Tile parallelism: if num_threads > 1 and row_mt disabled,
    // use tile-based parallelism
    if (thread_cfg->num_threads > 1 && !thread_cfg->enable_row_mt) {
      pbi->tile_mt_info.alloc_tile_rows = 1;
      pbi->tile_mt_info.alloc_tile_cols = thread_cfg->num_threads;
    }
  }
  
  // ... rest of initialization ...
}
```

**File: aom/aom_decoder.h**
Add thread configuration to init config:

```c
typedef struct aom_codec_dec_cfg {
  unsigned int threads;              // Maximum number of threads to use
  unsigned int w;                    // Width
  unsigned int h;                    // Height
  unsigned int allow_lowbitdepth;    // Allow low-bitdepth coding path
  // Console API additions:
  unsigned int enable_row_mt;        // Enable row-level multithreading
  unsigned int tile_parallel;        // Enable tile parallelism
} aom_codec_dec_cfg_t;
```

### Gotchas

1. **Thread count vs parallelism**: AOM's threading model is complex. `threads` controls total workers, but `row_mt` enables row-level parallelism. For console API, you may want to expose these as separate controls.

2. **Worker thread creation**: Creating workers is expensive. Don't re-create on every frame. The VP9 port kept workers alive across frames.

3. **Synchronization**: AOM uses barriers for worker synchronization. If you add your own copy thread, you need to coordinate with AOM's workers to avoid race conditions on output buffers.

4. **Memory usage**: More threads = more memory for thread-local data (MC buffers, etc.). The memory query function must account for this.

---

## Area 4: Decode Pipeline Split

### VP9 Pattern (from reports)
The critical split is between header parsing (which must be synchronous/non-blocking on the caller thread) and tile decoding (which can be asynchronous on workers). This ensures the caller can submit the next AU while the previous one is being decoded.

### AOM Equivalent
**Files involved:**
- `av1/decoder/obu.c` — OBU parsing: `aom_decode_frame_from_obus()`
- `av1/decoder/obu.h` — Function declarations
- `av1/decoder/decodeframe.c` — Tile decoding: `av1_decode_tg_tiles_and_wrapup()`
- `av1/decoder/decoder.c` — `av1_receive_compressed_data()`

**Key functions:**
- `aom_decode_frame_from_obus()` — Parses OBUs, decodes frame headers, sets up decode
- `av1_decode_tg_tiles_and_wrapup()` — Decodes tile groups (can be worker-parallel)
- `av1_decode_frame_headers_and_setup()` — Header parsing + preparation (stays on caller thread)

### Modifications Required

**File: av1/decoder/obu.h**
Expose the split functions for console API:

```c
// Parse OBUs and decode frame headers (synchronous, caller thread)
// Returns: 1 = frame decoded, 0 = no frame, -1 = error
int av1_decode_frame_headers_and_setup(AV1Decoder *pbi,
                                        const uint8_t *data,
                                        const uint8_t *data_end,
                                        const uint8_t **p_data_end);

// Decode tile groups (can be asynchronous if workers available)
// Must be called after header parsing completes
int av1_decode_tg_tiles_and_wrapup(AV1Decoder *pbi);
```

**File: av1/decoder/obu.c**
Modify to support the split:

```c
// This function already exists - it calls both header parse and tile decode
// For console API, we need to split these

int av1_console_decode_frame(AV1Decoder *pbi,
                              const uint8_t *data,
                              size_t size,
                              int *frame_decoded) {
  const uint8_t *data_end = data + size;
  
  // Phase 1: Header parsing (synchronous, caller thread)
  // This validates the bitstream and sets up decode context
  int parse_result = aom_decode_frame_from_obus(pbi, data, data_end, &data_end);
  
  if (parse_result < 0) {
    return -1;  // Error
  }
  
  if (parse_result == 0) {
    *frame_decoded = 0;
    return 0;  // No frame to decode
  }
  
  // At this point, frame headers are parsed and decode is set up
  // Tile decoding can now proceed (possibly async)
  
  // For console API, we return here and let tiles decode async
  // The caller will call av1_console_decode_tiles() later
  *frame_decoded = 1;
  return 0;
}

// Tile decoding (can be async)
int av1_console_decode_tiles(AV1Decoder *pbi) {
  // This is essentially what happens inside av1_decode_tg_tiles_and_wrapup
  // but we expose it separately for async pipeline
  
  if (pbi->num_workers > 1) {
    // Launch tile workers
    av1_launch_tile_workers(pbi);
    // Wait for completion
    av1_sync_tile_workers(pbi);
  } else {
    // Single-threaded decode
    av1_decode_tg_tiles_and_wrapup(pbi);
  }
  
  return 0;
}
```

**File: av1/decoder/decoder.c**
Modify `av1_receive_compressed_data()` to support split operation:

```c
int av1_receive_compressed_data(AV1Decoder *pbi, size_t size,
                                const uint8_t **psource) {
  // ... existing setup ...
  
  // For console API mode, we might want to return early after header parse
  // This allows the caller to submit next AU while tiles decode
  
  int frame_decoded = aom_decode_frame_from_obus(pbi, source, source + size, psource);
  
  if (frame_decoded < 0) {
    // Error
    return 1;
  }
  
  if (frame_decoded == 0) {
    // No frame decoded (e.g., only temporal delimiter)
    return 0;
  }
  
  // Frame headers parsed, tiles ready to decode
  // In console API, tiles will decode async
  // For now, we continue with synchronous decode
  
  // ... existing tile decode and buffer update ...
  
  return 0;
}
```

### Gotchas

1. **Header dependency**: Tile decoding cannot start until header parsing is complete. The split is safe because AV1 frame headers don't depend on tile data.

2. **Reference frame access**: If you're doing async tile decode, you must ensure reference frames are not modified while workers are reading them. AOM handles this via reference counting.

3. **Error propagation**: If header parsing finds an error, you must not proceed to tile decode. The error must propagate to the caller before any async work starts.

4. **State consistency**: After header parsing but before tile decode completes, the decoder is in a "pending" state. The console API must track this to prevent invalid operations (e.g., getting output before decode completes).

---

## Area 5: Queue / Pipeline Management

### VP9 Pattern (from reports)
The VP9 port used a job queue (ring buffer) between caller and worker threads. It tracked in-flight frames and had backpressure logic (QUEUE_FULL) when the pipeline was saturated.

### AOM Equivalent
**Files involved:**
- `av1/decoder/decoder.h` — `AV1DecTileMT` for tile job queue
- `av1/decoder/decoder.c` — Job queue management in tile workers
- `av1/decoder/decodeframe.c` — Worker job execution

**Key structures:**
- `AV1DecTileMT` — Tile job queue management
- `TileJobsDec` — Individual tile job
- `AV1DecRowMTInfo` — Row-level MT job tracking

### Modifications Required

**File: av1/decoder/decoder.h**
Add console API job queue tracking:

```c
typedef struct AV1Decoder {
  // ... existing fields ...
  
  // Console API: job queue tracking
  int console_api_mode;              // Enable console API job management
  uint32_t max_in_flight_frames;     // Max frames in pipeline
  uint32_t num_in_flight;            // Current frames in pipeline
  
  // Frame tracking for console API
  typedef struct ConsoleFrameJob {
    uint32_t frame_num;
    int pending;                     // 1 = headers parsed, tiles pending
    int complete;                    // 1 = fully decoded
    int error;                       // Error code if any
  } ConsoleFrameJob;
  
  ConsoleFrameJob *frame_jobs;
  uint32_t frame_jobs_size;
  uint32_t frame_jobs_head;
  uint32_t frame_jobs_tail;
  
  // ... rest of existing fields ...
} AV1Decoder;
```

**File: av1/decoder/decoder.c**
Implement job queue management:

```c
// Submit frame for async decode
int av1_console_submit_frame(AV1Decoder *pbi, 
                              const uint8_t *data, 
                              size_t size,
                              uint32_t *frame_id) {
  // Check backpressure
  if (pbi->num_in_flight >= pbi->max_in_flight_frames) {
    return AV1_DEC_ERR_QUEUE_FULL;
  }
  
  // Parse headers synchronously
  int result = aom_decode_frame_from_obus(pbi, data, data + size, &data);
  if (result <= 0) {
    return result < 0 ? AV1_DEC_ERR_BITSTREAM : AV1_DEC_SUCCESS;
  }
  
  // Allocate frame slot
  uint32_t slot = pbi->frame_jobs_tail;
  pbi->frame_jobs[slot].frame_num = pbi->frame_count++;
  pbi->frame_jobs[slot].pending = 1;
  pbi->frame_jobs[slot].complete = 0;
  pbi->frame_jobs[slot].error = 0;
  
  pbi->frame_jobs_tail = (slot + 1) % pbi->frame_jobs_size;
  pbi->num_in_flight++;
  
  if (frame_id) *frame_id = pbi->frame_jobs[slot].frame_num;
  
  // Launch async tile decode
  if (pbi->num_workers > 1) {
    av1_launch_tile_workers(pbi);
  } else {
    av1_decode_tg_tiles_and_wrapup(pbi);
    pbi->frame_jobs[slot].complete = 1;
  }
  
  return AV1_DEC_SUCCESS;
}

// Check if any frame is complete
int av1_console_poll_completed(AV1Decoder *pbi, 
                                uint32_t *completed_frame_id) {
  uint32_t slot = pbi->frame_jobs_head;
  
  while (slot != pbi->frame_jobs_tail) {
    if (pbi->frame_jobs[slot].complete) {
      if (completed_frame_id) {
        *completed_frame_id = pbi->frame_jobs[slot].frame_num;
      }
      
      // Remove from queue
      pbi->frame_jobs[slot].complete = 0;
      pbi->frame_jobs_head = (slot + 1) % pbi->frame_jobs_size;
      pbi->num_in_flight--;
      
      return 1;  // Found completed frame
    }
    
    // Check if async decode finished
    if (pbi->frame_jobs[slot].pending && pbi->num_workers > 1) {
      if (av1_tile_workers_done(pbi)) {
        pbi->frame_jobs[slot].complete = 1;
        pbi->frame_jobs[slot].pending = 0;
      }
    }
    
    slot = (slot + 1) % pbi->frame_jobs_size;
  }
  
  return 0;  // No completed frames
}
```

### Gotchas

1. **Queue size**: The VP9 port used a fixed-size ring buffer. For console API, you need to balance memory usage (queue size) against backpressure frequency.

2. **Frame ordering**: AV1 can output frames out of decode order (due to temporal dependencies). The console API should return frames in presentation order, which may require reordering logic.

3. **Worker synchronization**: If using AOM's internal workers, you need to check if they're done before marking a frame complete. Use `av1_worker_sync()`.

4. **Error handling**: If a frame in the middle of the pipeline errors, you need to decide whether to drain the rest or fail fast. VP9 chose to mark the error and continue.

---

## Area 6: Reference Frame / DPB Changes

### VP9 Pattern (from reports)
The VP9 port managed DPB slots explicitly. With async pipeline, reference frames must be held until all dependent frames are decoded, then released. AOM uses reference counting which handles this automatically.

### AOM Equivalent
**Files involved:**
- `av1/common/av1_common_int.h` — `AV1_COMMON` struct with DPB
- `av1/common/refcnt.h` — `RefCntBuffer` reference counting
- `av1/decoder/decoder.h` — `AV1Decoder` with `ref_frame_map`
- `av1/decoder/decoder.c` — `update_frame_buffers()` manages DPB

**Key structures:**
- `RefCntBuffer` — Reference-counted frame buffer
- `ref_frame_map[8]` — DPB slots in `AV1_COMMON`
- `cur_frame` — Current frame being decoded

### Modifications Required

**File: av1/common/av1_common_int.h**
Add console API DPB tracking:

```c
typedef struct AV1Common {
  // ... existing fields ...
  
  // Console API: track reference holdings for async pipeline
  int console_dpb_hold_refs;         // Don't release until explicitly told
  
  // ... rest of existing fields ...
} AV1Common;
```

**File: av1/decoder/decoder.c**
Modify `update_frame_buffers()` to support console API hold:

```c
static void update_frame_buffers(AV1Decoder *pbi, int frame_decoded) {
  AV1_COMMON *const cm = &pbi->common;
  BufferPool *const pool = cm->buffer_pool;
  
  if (frame_decoded) {
    lock_buffer_pool(pool);
    
    // Standard DPB update
    // ... (existing code) ...
    
    // Console API: if hold enabled, don't release reference
    // This keeps the frame in DPB until caller explicitly releases
    if (!cm->console_dpb_hold_refs) {
      // Normal release behavior
      // ... (existing code) ...
    }
    
    unlock_buffer_pool(pool);
  }
  
  // ... rest of function ...
}

// Console API: explicitly release reference
int av1_console_release_frame(AV1Decoder *pbi, int frame_idx) {
  AV1_COMMON *const cm = &pbi->common;
  BufferPool *const pool = cm->buffer_pool;
  
  if (frame_idx < 0 || frame_idx >= REF_FRAMES) {
    return -1;
  }
  
  RefCntBuffer *buf = cm->ref_frame_map[frame_idx];
  if (!buf) return 0;
  
  lock_buffer_pool(pool);
  decrease_ref_count(buf, pool);
  unlock_buffer_pool(pool);
  
  cm->ref_frame_map[frame_idx] = NULL;
  return 0;
}
```

### Gotchas

1. **Reference counting**: AOM's `decrease_ref_count()` automatically frees the buffer when count reaches zero. This is correct for normal operation.

2. **Async pipeline hazard**: With async decode, a frame might still be needed as reference while the next frame is being decoded. AOM's reference counting handles this—each frame that uses a reference increments the count.

3. **Output vs reference**: A frame can be both output (shown) and used as reference. The console API must not release a reference frame until all dependent frames are decoded.

4. **Film grain**: If film grain is applied, the grain parameters are stored in the frame buffer. The output copy must include grain parameters, not just pixel data.

---

## Area 7: Bitstream Parsing Changes

### VP9 Pattern (from reports)
OBU parsing must stay on the caller thread to satisfy non-blocking constraint. The Temporal Delimiter, Sequence Header, and Frame Header are parsed synchronously; Tile Groups can be parsed and decoded asynchronously.

### AOM Equivalent
**Files involved:**
- `av1/decoder/obu.c` — OBU parsing: `aom_decode_frame_from_obus()`, `av1_parse_obu_header()`
- `av1/decoder/obu.h` — OBU parsing functions
- `av1/decoder/decoder.h` — `DataBuffer` for OBU data

**Key functions:**
- `aom_decode_frame_from_obus()` — Main entry, parses all OBUs
- `av1_parse_obu_header()` — Parses OBU header (type, size)
- `av1_parse_sequence_header()` — Parses Sequence Header OBU
- `av1_parse_frame_header()` — Parses Frame Header OBU

### Modifications Required

**File: av1/decoder/obu.h**
Expose parsing functions for console API:

```c
// Parse OBU header only (no decode)
// Returns: 0 = success, -1 = error
int av1_parse_obu_header(const uint8_t *data, size_t size,
                          OBU_TYPE *obu_type, size_t *obu_size);

// Parse sequence header (synchronous)
// Returns: 0 = success, -1 = error  
int av1_parse_sequence_header(AV1Decoder *pbi, 
                               const uint8_t *data, 
                               size_t size);

// Parse frame header (synchronous)
// Returns: 0 = success, -1 = error
int av1_parse_frame_header(AV1Decoder *pbi,
                            const uint8_t *data,
                            size_t size,
                            size_t *header_size);
```

**File: av1/decoder/obu.c**
Add console API parsing entry point:

```c
// Console API: synchronous OBU parsing
// This is what runs on the caller thread before async tile decode
int av1_console_parse_obus(AV1Decoder *pbi,
                           const uint8_t *data,
                           size_t size,
                           int *is_keyframe,
                           int *frame_decoded) {
  const uint8_t *data_end = data + size;
  
  // Parse OBUs until we hit a frame
  while (data < data_end) {
    OBU_TYPE obu_type;
    size_t obu_size;
    
    if (av1_parse_obu_header(data, data_end - data, &obu_type, &obu_size) < 0) {
      return AV1_DEC_ERR_OBU_ERROR;
    }
    
    switch (obu_type) {
      case OBU_TEMPORAL_DELIMITER:
        // No data, just skip
        break;
        
      case OBU_SEQUENCE_HEADER:
        // Parse sequence header (sets up decoder config)
        if (av1_parse_sequence_header(pbi, data, obu_size) < 0) {
          return AV1_DEC_ERR_OBU_ERROR;
        }
        break;
        
      case OBU_FRAME_HEADER:
        // Parse frame header (sets up current frame decode)
        if (av1_parse_frame_header(pbi, data, obu_size, NULL) < 0) {
          return AV1_DEC_ERR_FRAME_ERROR;
        }
        break;
        
      case OBU_FRAME:
      case OBU_TILE_GROUP:
        // Frame data - stop parsing, return to caller
        // Tile decode can proceed asynchronously
        *frame_decoded = 1;
        *is_keyframe = (pbi->common.frame_type == KEY_FRAME);
        return 0;
        
      default:
        // Skip unknown OBUs
        break;
    }
    
    data += obu_size;
  }
  
  *frame_decoded = 0;
  return 0;
}
```

### Gotchas

1. **Sequence header changes**: If the sequence header changes mid-stream (resolution, profile, etc.), AOM reinitializes internal buffers. The console API must handle this and may need to flush pending frames.

2. **Temporal delimiter**: Every temporal unit starts with a temporal delimiter OBU. This is important for console API to detect frame boundaries.

3. **Tile group parsing**: The tile group OBU contains the actual compressed data. Parsing the tile group header (to get tile positions) can be done synchronously, but tile data decode is async.

4. **Annex-B vs IVF**: The console API must handle both. Annex-B has explicit start codes; IVF has frame sizes in the container. The wrapper detects format and handles appropriately.

---

## Area 8: Post-Processing / Filtering

### VP9 Pattern (from reports)
Loop filter, CDEF, and loop restoration run on worker threads in AOM. Film grain application happens on the output buffer (not in DPB). The console API needs to ensure these complete before returning output.

### AOM Equivalent
**Files involved:**
- `av1/common/av1_loopfilter.h` — Loop filter
- `av1/common/cdef.h` — CDEF (Constrained Directional Enhancement Filter)
- `av1/common/loop_restoration.h` — Loop restoration
- `av1/decoder/decoder.h` — `AV1LfSync`, `AV1CdefSync`, `AV1LrSync`
- `av1/decoder/decodeframe.c` — Post-processing in frame decode

**Key functions:**
- `av1_loop_filter_frame()` — Apply loop filter
- `av1_cdef_frame()` — Apply CDEF
- `av1_loop_restoration_save_deblock()` — Prepare for restoration
- `av1_loop_restoration_filter_frame()` — Apply restoration

### Modifications Required

**File: av1/decoder/decoder.h**
Add post-processing tracking to `AV1Decoder`:

```c
typedef struct AV1Decoder {
  // ... existing fields ...
  
  // Console API: post-processing state
  int postproc_pending;              // Post-processing not yet complete
  int postproc_lf_done;              // Loop filter done
  int postproc_cdef_done;            // CDEF done
  int postproc_lr_done;              // Loop restoration done
  
  // ... rest of existing fields ...
} AV1Decoder;
```

**File: av1/decoder/decodeframe.c**
Ensure post-processing completes before frame is outputtable:

```c
// In the frame decode function, after tile decode:
// This is already how AOM works - postproc is part of frame decode

static void finish_frame_decode(AV1Decoder *pbi) {
  AV1_COMMON *cm = &pbi->common;
  
  // Loop filter
  if (cm->loop_filter_level[0] || cm->loop_filter_level[1]) {
    av1_loop_filter_frame(cm, &pbi->lf_row_sync, 0, 1);
  }
  
  // CDEF
  if (cm->cdef_enabled) {
    av1_cdef_frame(cm, pbi->cdef_worker, pbi->cdef_sync, pbi->num_workers);
  }
  
  // Loop restoration
  if (cm->restoration_type != RESTORE_NONE) {
    av1_loop_restoration_filter_frame(cm, &pbi->lr_row_sync, 0);
  }
  
  // Film grain (if present) is applied during output copy, not stored in DPB
  // This is correct - grain is not reference-worthy
}

// Console API: ensure post-processing is complete before output
int av1_console_wait_postproc(AV1Decoder *pbi) {
  // If using workers, sync them
  if (pbi->num_workers > 1) {
    // Workers handle post-proc in parallel
    av1_sync_all_workers(pbi);
  }
  
  // Film grain application happens at output time
  // (not in DPB, applied to output buffer)
  
  return 0;
}
```

### Gotchas

1. **Parallel post-processing**: Loop filter, CDEF, and restoration can run in parallel with tile decode for subsequent frames. AOM handles this internally.

2. **Film grain timing**: Film grain is NOT stored in the DPB reference frames. It's applied when the frame is output. This is correct per AV1 spec—grain is not reference-worthy.

3. **Output buffer vs DPB**: The console API output must be a copy, not a reference to the DPB buffer, because the DPB buffer may be reused for subsequent frames.

4. **Skip film grain**: Some applications want to skip film grain for performance. The console API should have an option to skip grain application on output.

---

## Area 9: Output / Copy Path

### VP9 Pattern (from reports)
The console API has SET OUTPUT (register external buffer) and RECEIVE OUTPUT (copy decoded frame to buffer). This includes plane-by-plane copy, format conversion, and film grain application.

### AOM Equivalent
**Files involved:**
- `av1/decoder/decoder.c` — `av1_get_raw_frame()`, `av1_get_frame_to_show()`
- `aom/aom_image.h` — `aom_image_t` structure
- `aom_scale/yv12config.h` — `YV12_BUFFER_CONFIG` structure

**Key structures:**
- `aom_image_t` — AOM's image format (planes, strides, bit depth)
- `YV12_BUFFER_CONFIG` — Internal frame buffer format

### Modifications Required

**File: av1/decoder/decoder.h**
Add console API output management:

```c
typedef struct AV1Decoder {
  // ... existing fields ...
  
  // Console API: output buffer management
  void *ext_output_buffer;           // Caller-provided output buffer
  size_t ext_output_size;            // Buffer size
  int output_format;                 // Output format (YUV420, NV12, etc.)
  
  // Film grain
  int apply_film_grain;              // Apply film grain to output
  
  // ... rest of existing fields ...
} AV1Decoder;
```

**File: av1/decoder/decoder.c**
Implement console API output functions:

```c
// Set external output buffer
int av1_console_set_output(AV1Decoder *pbi, 
                           void *buffer, 
                           size_t size,
                           int format) {
  pbi->ext_output_buffer = buffer;
  pbi->ext_output_size = size;
  pbi->output_format = format;
  return 0;
}

// Get output frame (copy to external buffer)
int av1_console_get_output(AV1Decoder *pbi,
                           Av1DecOutput *output) {
  YV12_BUFFER_CONFIG *frame = NULL;
  aom_film_grain_t *grain = NULL;
  
  // Get frame from output queue
  if (av1_get_raw_frame(pbi, 0, &frame, &grain) < 0) {
    return AV1_DEC_ERR_NO_OUTPUT;
  }
  
  if (!frame) {
    return AV1_DEC_SUCCESS;  // No output available
  }
  
  // Verify buffer size
  size_t required_size = av1_calculate_output_size(frame, pbi->output_format);
  if (required_size > pbi->ext_output_size) {
    return AV1_DEC_ERR_BUF_TOO_SMALL;
  }
  
  // Copy frame to output buffer
  if (pbi->output_format == AV1_DEC_OUTPUT_YUV420) {
    copy_yuv420(frame, pbi->ext_output_buffer);
  } else if (pbi->output_format == AV1_DEC_OUTPUT_NV12) {
    copy_yuv420_to_nv12(frame, pbi->ext_output_buffer);
  }
  
  // Apply film grain if requested and present
  if (pbi->apply_film_grain && grain && grain->apply_grain) {
    apply_film_grain(frame, grain, pbi->ext_output_buffer, pbi->output_format);
  }
  
  // Fill output info
  output->width = frame->y_width;
  output->height = frame->y_height;
  output->pts = frame->pts;
  output->timestamp = frame->timestamp;
  
  return 1;  // Frame returned
}

// Helper: calculate required output buffer size
size_t av1_calculate_output_size(YV12_BUFFER_CONFIG *frame, int format) {
  size_t y_size = frame->y_stride * frame->y_height;
  size_t uv_size = frame->uv_stride * frame->uv_height;
  
  if (format == AV1_DEC_OUTPUT_YUV420) {
    return y_size + 2 * uv_size;
  } else if (format == AV1_DEC_OUTPUT_NV12) {
    // NV12 has Y plane + interleaved UV
    return y_size + frame->uv_stride * frame->uv_height * 2;
  }
  
  return y_size + 2 * uv_size;
}

// Copy YUV420 planar
static void copy_yuv420(YV12_BUFFER_CONFIG *src, uint8_t *dst) {
  // Y plane
  for (int y = 0; y < src->y_height; y++) {
    memcpy(dst + y * src->y_width,
           src->y_buffer + y * src->y_stride,
           src->y_width);
  }
  
  // U plane
  uint8_t *dst_u = dst + src->y_width * src->y_height;
  for (int y = 0; y < src->uv_height; y++) {
    memcpy(dst_u + y * src->uv_width,
           src->u_buffer + y * src->uv_stride,
           src->uv_width);
  }
  
  // V plane
  uint8_t *dst_v = dst_u + src->uv_width * src->uv_height;
  for (int y = 0; y < src->uv_height; y++) {
    memcpy(dst_v + y * src->uv_width,
           src->v_buffer + y * src->uv_stride,
           src->uv_width);
  }
}

// Copy YUV420 to NV12 (semi-planar)
static void copy_yuv420_to_nv12(YV12_BUFFER_CONFIG *src, uint8_t *dst) {
  // Y plane (same as YUV420)
  for (int y = 0; y < src->y_height; y++) {
    memcpy(dst + y * src->y_width,
           src->y_buffer + y * src->y_stride,
           src->y_width);
  }
  
  // UV interleaved
  uint8_t *dst_uv = dst + src->y_width * src->y_height;
  for (int y = 0; y < src->uv_height; y++) {
    uint8_t *src_u = src->u_buffer + y * src->uv_stride;
    uint8_t *src_v = src->v_buffer + y * src->uv_stride;
    uint8_t *dst_row = dst_uv + y * src->uv_width * 2;
    
    for (int x = 0; x < src->uv_width; x++) {
      dst_row[x * 2] = src_u[x];
      dst_row[x * 2 + 1] = src_v[x];
    }
  }
}
```

### Gotchas

1. **Output queue**: AOM can have multiple frames queued (for SVC or output_all_layers). The console API must handle this—use `av1_get_raw_frame(pbi, index, ...)` to iterate.

2. **Buffer release**: After copying output, the frame is still in the DPB. You must call the release function to free the reference, or let it be released on the next decode call.

3. **Film grain timing**: Film grain is applied at output time, not stored in DPB. The grain parameters come from `av1_get_raw_frame()` and must be applied to the output buffer.

4. **Format conversion**: AOM internally uses YV12 (planar Y, V, U). Console API output may want NV12 (semi-planar) or other formats. The copy function handles conversion.

---

## Area 10: Error Handling & Edge Cases

### VP9 Pattern (from reports)
The VP9 port handled resolution changes (sequence header changes), flush drain, destroy cleanup, and error propagation from worker threads. AOM has built-in error handling via `setjmp/longjmp` and `aom_internal_error_info`.

### AOM Equivalent
**Files involved:**
- `aom/internal/aom_codec_internal.h` — `aom_internal_error_info` struct
- `av1/decoder/decoder.h` — `error` field in `AV1Decoder`
- `av1/decoder/decoder.c` — Error handling in `av1_receive_compressed_data()`
- `av1/decoder/obu.c` — OBU parsing errors

**Key structures:**
- `aom_internal_error_info` — Error code and detail
- `aom_codec_err_t` — Error codes (`AOM_CODEC_OK`, `AOM_CODEC_CORRUPT_FRAME`, etc.)

### Modifications Required

**File: av1/decoder/decoder.h**
Add console API error tracking:

```c
typedef struct AV1Decoder {
  // ... existing fields ...
  
  // Console API: error tracking
  int console_error;                 // Last error code
  char console_error_detail[256];    // Error detail string
  
  // Sequence header change tracking
  int seq_header_changed;            // Reset required due to config change
  
  // ... rest of existing fields ...
} AV1Decoder;
```

**File: av1/decoder/decoder.c**
Implement console API error handling:

```c
// Get last error
int av1_console_get_error(Av1ConsoleDecoder *dec, 
                          char *detail, 
                          size_t detail_size) {
  if (!dec) return AV1_DEC_ERR_INVALID_PARAM;
  
  int err = dec->console_error;
  
  if (detail && detail_size > 0) {
    strncpy(detail, dec->console_error_detail, detail_size - 1);
    detail[detail_size - 1] = '\0';
  }
  
  return err;
}

// Handle sequence header change (resolution change)
int av1_console_handle_seq_change(AV1Decoder *pbi) {
  AV1_COMMON *cm = &pbi->common;
  
  // Check if sequence header changed
  if (pbi->sequence_header_changed) {
    // Reallocate buffers for new resolution
    if (av1_alloc_context_buffers(cm, cm->width, cm->height,
                                   cm->mi_params.min_partition_size) < 0) {
      return AV1_DEC_ERR_MEM_ALLOC;
    }
    
    // Reset frame count
    pbi->frame_count = 0;
    pbi->sequence_header_changed = 0;
    
    // Clear output queue
    pbi->num_output_frames = 0;
  }
  
  return 0;
}

// Flush: drain all pending output
int av1_console_flush(Av1ConsoleDecoder *dec) {
  if (!dec) return AV1_DEC_ERR_INVALID_PARAM;
  
  dec->state = AV1_DEC_CONSOLE_STATE_FLUSHING;
  
  // Call decode with NULL data to signal end of stream
  aom_codec_err_t err = aom_codec_decode(&dec->aom_ctx, NULL, 0, NULL);
  
  // Drain any remaining frames
  aom_codec_iter_t iter = NULL;
  const aom_image_t *img;
  while ((img = aom_codec_get_frame(&dec->aom_ctx, &iter)) != NULL) {
    // Process each frame
  }
  
  return av1ConsoleDecMapError(err);
}

// Reset: reinitialize decoder state
int av1_console_reset(Av1ConsoleDecoder *dec) {
  if (!dec) return AV1_DEC_ERR_INVALID_PARAM;
  
  // Destroy and recreate
  aom_codec_destroy(&dec->aom_ctx);
  
  // Reinitialize
  aom_codec_dec_cfg_t cfg = {
    .threads = dec->num_threads,
    .w = 0,
    .h = 0,
    .allow_lowbitdepth = 1
  };
  
  extern aom_codec_iface_t *aom_codec_av1_dx(void);
  aom_codec_err_t err = aom_codec_dec_init(&dec->aom_ctx, 
                                            aom_codec_av1_dx(), 
                                            &cfg, 0);
  if (err != AOM_CODEC_OK) {
    return AV1_DEC_ERR_MEM_ALLOC;
  }
  
  dec->pbi = (AV1Decoder *)dec->aom_ctx.priv;
  dec->state = AV1_DEC_CONSOLE_STATE_CREATED;
  dec->frame_count = 0;
  
  return AV1_DEC_SUCCESS;
}

// Error mapping from AOM to console API
static int av1ConsoleDecMapError(aom_codec_err_t aom_err) {
  switch (aom_err) {
    case AOM_CODEC_OK:
      return AV1_DEC_SUCCESS;
    case AOM_CODEC_ERROR:
      return AV1_DEC_ERR_GENERIC;
    case AOM_CODEC_MEM_ERROR:
      return AV1_DEC_ERR_MEM_ALLOC;
    case AOM_CODEC_UNSUP_BITSTREAM:
      return AV1_DEC_ERR_UNSUP_BITSTREAM;
    case AOM_CODEC_UNSUP_FEATURE:
      return AV1_DEC_ERR_UNSUP_FEATURE;
    case AOM_CODEC_CORRUPT_FRAME:
      return AV1_DEC_ERR_CORRUPT_FRAME;
    case AOM_CODEC_INVALID_PARAM:
      return AV1_DEC_ERR_INVALID_PARAM;
    default:
      return AV1_DEC_ERR_GENERIC;
  }
}
```

### Gotchas

1. **setjmp/longjmp**: AOM uses `setjmp/longjmp` for error handling. If you integrate with your own threading, be careful—longjmp can bypass normal stack unwinding.

2. **Error propagation from workers**: If a worker thread encounters an error, it sets the error in `pbi->error`. The main thread must check this after syncing workers.

3. **Resolution change mid-stream**: When sequence header changes, AOM reinitializes. The console API must detect this and may need to flush pending frames before continuing.

4. **Flush behavior**: Calling `aom_codec_decode()` with NULL data signals end of stream. This flushes any pending frames but doesn't reset the decoder.

5. **Memory leaks on error**: If an error occurs during decode, ensure all allocated resources are freed. AOM's `av1_decoder_remove()` handles this, but if you're managing your own state, you need cleanup code.

---

## Summary

This guide provides the concrete modifications needed to add a console-style API to the AOM AV1 reference decoder. The key patterns from the VP9 port that apply to AV1 are:

1. **State machine**: Track decoder state (CREATED, DECODING, FLUSHING) in a wrapper struct
2. **Split pipeline**: Header parsing stays synchronous; tile decode can be async
3. **Iterator pattern**: Use `aom_codec_get_frame()` iterator, not a queue
4. **External buffers**: Use AOM's frame buffer callback API for external memory management
5. **Error mapping**: Convert AOM error codes to console API codes
6. **Thread config**: Expose threading options (row_mt, tile parallelism) in config

The modifications are spread across:
- `av1/decoder/decoder.h` — New wrapper struct and tracking fields
- `av1/decoder/decoder.c` — Console API functions
- `av1/decoder/obu.c` — Exposed parsing functions for split pipeline
- `aom/aom_decoder.h` — Extended config structures
- `aom/aom_frame_buffer.h` — Memory query functions