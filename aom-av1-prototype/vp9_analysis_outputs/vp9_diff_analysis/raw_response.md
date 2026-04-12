

# VP9 to AV1 Decoder Diff Analysis

## Progress
- [x] 1. Public API Layer — analyzed
- [x] 2. Memory Management — analyzed
- [x] 3. Threading Architecture — analyzed
- [x] 4. Decode Pipeline Split (Non-Blocking DECODE) — analyzed
- [x] 5. Queue / Pipeline Management — analyzed
- [x] 6. Reference Frame / DPB Changes — analyzed
- [x] 7. Bitstream Parsing Changes — analyzed
- [x] 8. Post-Processing / Filtering — analyzed
- [x] 9. Output / Copy Path — analyzed
- [x] 10. Error Handling & Edge Cases — analyzed

---

## 1. Public API Layer

### Reference (libvpx VP9)

The reference uses the standard libvpx API:

```c
// vpx/vpx_decoder.h
vpx_codec_err_t vpx_codec_dec_init_ver(vpx_codec_ctx_t *ctx, vpx_codec_iface_t *iface,
                                        const vpx_codec_dec_cfg_t *cfg, vpx_codec_flags_t flags, int ver);
vpx_codec_err_t vpx_codec_decode(vpx_codec_ctx_t *ctx, const uint8_t *data,
                                  unsigned int data_sz, void *user_priv, long deadline);
vpx_image_t *vpx_codec_get_frame(vpx_codec_ctx_t *ctx, vpx_codec_iter_t *iter);
```

The VP9 decoder interface is accessed through `vpx_codec_iface_t` with standard capabilities:
- `VPX_CODEC_CAP_FRAME_THREADING` - frame-based multi-threading
- `VPX_CODEC_CAP_EXTERNAL_FRAME_BUFFER` - external frame buffers
- `VPX_CODEC_CAP_HIGHBITDEPTH` - 10/12-bit depth support

### Custom (AV1 AOM)

The custom implements a completely new AV1-specific API:

```c
// av1_aom_dec.h (from 17_compile_final_plan.md)
typedef enum _Av1AomDecReturnType {
    AV1_AOM_DEC_SUCCESS = 0,
    AV1_AOM_DEC_ERR_INVALID_PARAM = -1,
    AV1_AOM_DEC_ERR_INVALID_CTX = -2,
    AV1_AOM_DEC_ERR_MEM_ALLOC = -3,
    AV1_AOM_DEC_ERR_UNSUP_BITSTREAM = -4,
    AV1_AOM_DEC_ERR_UNSUP_FEATURE = -5,
    AV1_AOM_DEC_ERR_CORRUPT_FRAME = -6,
    AV1_AOM_DEC_ERR_TIMEOUT = -7,  // kept for compatibility but unused
    AV1_AOM_DEC_ERR_BUF_TOO_SMALL = -8,
    AV1_AOM_DEC_ERR_OBU_ERROR = -25,   // AV1-specific
    AV1_AOM_DEC_ERR_TILE_ERROR = -26,  // AV1-specific
} Av1AomDecReturnType;
```

New AV1-specific functions:
- `av1AomDecCreate()` - creates decoder instance
- `av1AomDecDecodeAu()` - decodes AV1 Access Unit
- `av1AomDecSyncAu()` - synchronizes AU decode completion
- `av1AomDecSetDecodeOutput()` - sets external output buffer
- `av1AomDecSyncDecodeOutput()` - syncs output
- `av1AomDecFlush()` - flush (note: not directly supported by AOM)
- `av1AomDecReset()` - reset decoder state
- `av1AomDecQueryMemory()` - query memory requirements
- `av1AomDecQueryFrameSize()` - query frame buffer size
- `av1AomDecQueryInstanceCharacteristics()` - query decoder capabilities

### Key Files Changed

| File | Change Type | Description |
|------|-------------|-------------|
| `av1_aom_dec.h` | NEW | Complete AV1 API header |
| `av1api_dec_create.cpp` | NEW | Decoder creation using `aom_codec_dec_init_ver()` |
| `av1api_decode_au.cpp` | NEW | AU decoding using `aom_codec_decode()` |
| `av1api_sync_au.cpp` | NEW | Frame retrieval using `aom_codec_get_frame()` |
| `av1api_destroy.cpp` | NEW | Cleanup using `aom_codec_destroy()` |

### Diff Summary

```diff
- vpx_codec_dec_init(ctx, &vpx_codec_vp9_dx, &cfg, flags)
- vpx_codec_decode(ctx, data, size, priv, 0)
- vpx_codec_get_frame(ctx, &iter)

+ av1AomDecCreate(&instance_desc, &context)
+ av1AomDecDecodeAu(context, data, size, &control)
+ av1AomDecSyncAu(context, &result)
```

### Design Rationale

The custom API wraps libaom to provide a platform-specific interface that:
1. **Hides AOM specifics** - Applications don't need to know about `aom_codec_ctx_t`
2. **Adds AV1-specific error codes** - OBU_ERROR and TILE_ERROR for AV1-specific failures
3. **Maintains VP9-like semantics** - Create → Decode → Sync pattern preserved
4. **Removes unsupported features** - GPU sync, thread priority, kernel affinity removed (not in AOM)

---

## 2. Memory Management

### Reference (libvpx VP9)

VP9 uses custom memory allocation:

```c
// vpx_mem/vpx_mem.h
void *vpx_memalign(size_t align, size_t size);
void *vpx_malloc(size_t size);
void *vpx_calloc(size_t num, size_t size);
void vpx_free(void *memblk);

// vp9_decoder.c - decoder instance allocation
VP9Decoder *vp9_decoder_create(BufferPool *const pool) {
  VP9Decoder *volatile const pbi = vpx_memalign(32, sizeof(*pbi));
  // ... frame context allocation
  CHECK_MEM_ERROR(&cm->error, cm->fc, vpx_calloc(1, sizeof(*cm->fc)));
  CHECK_MEM_ERROR(&cm->error, cm->frame_contexts, vpx_calloc(FRAME_CONTEXTS, ...));
}
```

VP9 row-based multi-threading memory:
```c
// vp9_decoder.c
void vp9_dec_alloc_row_mt_mem(RowMTWorkerData *row_mt_worker_data,
                              VP9_COMMON *cm, int num_sbs, int max_threads, int num_jobs) {
  const size_t dqcoeff_size = ((size_t)num_sbs << DQCOEFFS_PER_SB_LOG2) *
                              sizeof(*row_mt_worker_data->dqcoeff[0]);
  // Per-plane dqcoeff allocation with 32-byte alignment
  for (int plane = 0; plane < 3; ++plane) {
    CHECK_MEM_ERROR(&cm->error, row_mt_worker_data->dqcoeff[plane],
                    vpx_memalign(32, dqcoeff_size));
  }
  // EOB, partition, recon_map allocations
}
```

### Custom (AV1 AOM)

AV1 uses AOM's internal memory management plus custom wrapper:

```c
// av1_internals.h (from 16_plan_internals.md)
typedef struct _Av1AomDecoder {
    aom_codec_ctx_t ctx;
    Av1AomDecStreamDesc stream_desc;
    Av1AomDecInstanceDesc instance_desc;
    Av1AomDecState state;
    
    void* pending_output_buffer;
    uint32_t pending_output_size;
    int has_pending_frame;
    
    aom_get_frame_buffer_cb_fn_t fb_get_cb;
    aom_release_frame_buffer_cb_fn_t fb_release_cb;
    void* cb_priv;
    
    uint32_t frame_count;
    int uses_annex_b;
} Av1AomDecoder;
```

Memory query function:
```c
// 17_compile_final_plan.md - av1api_query_dec_memory.cpp
extern Av1AomDecReturnType av1AomDecQueryMemory(
    const Av1AomDecStreamDesc* pStreamDesc,
    uint32_t maxExtraDisplayFrameNum,
    Av1AomDecInstanceMemoryDesc* pMemDesc);
```

### Key Files Changed

| File | Change Type | Description |
|------|-------------|-------------|
| `vpx_mem/vpx_mem.c` | REFERENCE | Custom aligned allocator with overflow checking |
| `av1_internals.h/cpp` | NEW | AV1 decoder wrapper struct |
| `av1api_query_dec_memory.cpp` | NEW | Memory query using AOM |

### Diff Summary

```diff
- VP9Decoder* pbi = vpx_memalign(32, sizeof(*pbi));
- vpx_calloc(FRAME_CONTEXTS, sizeof(FRAME_CONTEXT));
- vp9_dec_alloc_row_mt_mem(...);  // row-MT specific allocations

+ Av1AomDecoder* dec = (Av1AomDecoder*)malloc(sizeof(Av1AomDecoder));
+ // Uses AOM internal memory management
+ av1AomDecQueryMemory(...) // queries AOM for memory requirements
```

### Design Rationale

1. **Removed custom allocators** - AOM handles internal memory
2. **Simplified DPB management** - No need for custom row-MT memory (AOM handles threading internally)
3. **External frame buffer support** - Added `fb_get_cb`/`fb_release_cb` for application-provided buffers
4. **Memory query API** - Added to help applications allocate sufficient memory

---

## 3. Threading Architecture

### Reference (libvpx VP9)

VP9 has sophisticated multi-threading:

```c
// vp9_decoder.h
typedef struct VP9Decoder {
  VPxWorker lf_worker;           // loop filter worker
  VPxWorker *tile_workers;       // tile-level workers
  TileWorkerData *tile_worker_data;
  int num_tile_workers;
  int total_tiles;
  
  VP9LfSync lf_row_sync;         // loop filter sync
  
  int row_mt;                    // row-based multi-threading flag
  int lpf_mt_opt;                // loop filter MT optimization
  RowMTWorkerData *row_mt_worker_data;
} VP9Decoder;
```

Thread synchronization:
```c
// vp9_decoder.c - job queue for row-MT
typedef struct Job {
  int row_num;
  int tile_col;
  JobType job_type;  // PARSE_JOB, RECON_JOB, LPF_JOB
} Job;

// vp9_job_queue.h - job queue management
typedef struct JobQueueRowMt {
  Job *job_queue;
  int32_t *job_status;
  int32_t write_idx;
  int32_t read_idx;
} JobQueueRowMt;
```

Worker thread interface:
```c
// vpx_util/vpx_thread.h
typedef struct {
  void (*init)(VPxWorker *const worker);
  int (*reset)(VPxWorker *const worker);
  int (*sync)(VPxWorker *const worker);
  void (*launch)(VPxWorker *const worker);
  void (*execute)(VPxWorker *const worker);
  void (*end)(VPxWorker *const worker);
} VPxWorkerInterface;
```

### Custom (AV1 AOM)

AV1 delegates threading to AOM internally:

```c
// 17_compile_final_plan.md
typedef struct _Av1AomDecInstanceDesc {
    Av1AomDecMemoryDesc memory;
    Av1AomDecInstanceMemoryType memoryType;
    uint32_t maxExtraDisplayFrameNum;
    uint32_t decodingTaskQueueDepth;
    int autoSyncFlag;
    uint32_t numThreads;  // Controls tile-level parallelism
    uint32_t decoderVersion;
} Av1AomDecInstanceDesc;
```

**Removed threading features:**
- No custom worker threads (AOM handles internally)
- No job queue (AOM manages internally)
- No row-based MT (handled by AOM)
- No loop filter threading (handled by AOM)

### Key Files Changed

| File | Change Type | Description |
|------|-------------|-------------|
| `vpx_util/vpx_thread.h/c` | REFERENCE | Worker thread interface |
| `vp9/decoder/vp9_job_queue.h` | REFERENCE | Row-MT job queue |
| `vp9/common/vp9_thread_common.h` | REFERENCE | Loop filter sync |
| `av1_tile_dec_unit.h` | NEW | AV1 tile decode unit parsing |

### Diff Summary

```diff
- VPxWorker lf_worker;
- VPxWorker *tile_workers;
- JobQueueRowMt jobq;
- pthread_mutex_t recon_done_mutex;
- RowMTWorkerData *row_mt_worker_data;

+ uint32_t numThreads;  // passed to AOM at init time
+ // All threading handled internally by AOM
```

### Design Rationale

1. **Removed custom threading** - AOM has its own internal threading model
2. **Simplified thread control** - Only `numThreads` parameter exposed to application
3. **Removed platform-specific features** - No kernel thread affinity, no priority control
4. **Tile parallelism** - AOM uses tile-based parallelism internally

### Potential Issues

**BUG RISK**: The custom API doesn't expose AOM's threading behavior. If the application needs to:
- Know when decoding is actually complete
- Control thread affinity
- Handle thread-local storage

This is not possible with the current design. The `av1AomDecSyncAu()` call may block indefinitely if AOM's internal threads deadlock.

---

## 4. Decode Pipeline Split (Non-Blocking DECODE)

### Reference (libvpx VP9)

VP9 uses a split decode/sync model:

```c
// vp9_decoder.c
int vp9_receive_compressed_data(VP9Decoder *pbi, size_t size, const uint8_t **psource) {
  // ... find free frame buffer ...
  vp9_decode_frame(pbi, source, source + size, psource);  // decode
  swap_frame_buffers(pbi);  // swap for output
  return retcode;
}

int vp9_get_raw_frame(VP9Decoder *pbi, YV12_BUFFER_CONFIG *sd, vp9_ppflags_t *flags) {
  // Returns decoded frame if available
  if (pbi->ready_for_new_data == 1) return ret;  // no frame
  *sd = *cm->frame_to_show;
  ret = 0;
}
```

The decode is blocking in `vp9_receive_compressed_data()`, but `vp9_get_raw_frame()` can be called later to retrieve the result.

### Custom (AV1 AOM)

AV1 has explicit AU decode/sync split:

```c
// 17_compile_final_plan.md
typedef struct _Av1AomDecAuDecodingControl {
    void* callerData;
    uint64_t ptsData;
    uint64_t dtsData;
} Av1AomDecAuDecodingControl;

typedef struct _Av1AomDecAuResult {
    int decodeStatus;
    uint32_t decodedFrameNum;
    uint64_t ptsData;
    uint64_t dtsData;
} Av1AomDecAuResult;

// Decode AU - may return before decode is complete
extern Av1AomDecReturnType av1AomDecDecodeAu(
    void* pContext,
    const uint8_t* pData,
    uint32_t size,
    const Av1AomDecAuDecodingControl* pControl);

// Sync AU - blocks until decode is complete
extern Av1AomDecReturnType av1AomDecSyncAu(
    void* pContext,
    Av1AomDecAuResult* pResult);
```

### Key Files Changed

| File | Change Type | Description |
|------|-------------|-------------|
| `vp9/decoder/vp9_decoder.c` | REFERENCE | `vp9_receive_compressed_data()` / `vp9_get_raw_frame()` |
| `av1api_decode_au.cpp` | NEW | AV1 AU decode |
| `av1api_sync_au.cpp` | NEW | AV1 AU sync |

### Diff Summary

```diff
- vp9_receive_compressed_data(pbi, size, &source);
- // decode happens here, blocking
- vp9_get_raw_frame(pbi, &sd, &flags);

+ av1AomDecDecodeAu(ctx, data, size, &control);
+ // decode submitted, may not be complete
+ av1AomDecSyncAu(ctx, &result);
+ // now decode is complete
```

### Design Rationale

1. **Explicit split** - `DecodeAu` submits work, `SyncAu` waits for completion
2. **PTS/DTS support** - Added `ptsData`/`dtsData` in control and result structs
3. **Status reporting** - `decodeStatus` in result indicates success/failure

---

## 5. Queue / Pipeline Management

### Reference (libvpx VP9)

VP9 has a job queue for row-based multi-threading:

```c
// vp9_job_queue.h
typedef struct JobQueueRowMt {
  Job *job_queue;
  int32_t *job_status;
  int32_t write_idx;
  int32_t read_idx;
  int32_t pending_jobs;
#if CONFIG_MULTITHREAD
  pthread_mutex_t mutex;
  pthread_cond_t cv;
#endif
} JobQueueRowMt;

// Job types
typedef enum JobType { PARSE_JOB, RECON_JOB, LPF_JOB } JobType;
```

Jobs are enqueued during tile parsing and dequeued by worker threads:
```c
// vp9_decodeframe.c - job enqueue during decode
void vp9_decode_frame(VP9Decoder *pbi, ...) {
  // ... parse tiles ...
  // Enqueue parse jobs
  // Enqueue recon jobs  
  // Enqueue loop filter jobs
}
```

### Custom (AV1 AOM)

AV1 has no explicit job queue - AOM handles internally:

```c
// 17_compile_final_plan.md
typedef struct _Av1AomDecInstanceDesc {
    // ...
    uint32_t decodingTaskQueueDepth;  // Not used - placeholder
    // ...
} Av1AomDecInstanceDesc;
```

The `decodingTaskQueueDepth` field exists but is not actually used - it's a placeholder for compatibility.

### Key Files Changed

| File | Change Type | Description |
|------|-------------|-------------|
| `vp9/decoder/vp9_job_queue.h` | REFERENCE | Job queue implementation |
| `vp9/decoder/vp9_job_queue.c` | REFERENCE | Job queue functions |

### Diff Summary

```diff
- JobQueueRowMt jobq;
- vp9_jobq_init(&jobq, num_jobs, jobq_buf, jobq_size);
- vp9_jobq_enqueue(&jobq, PARSE_JOB, row, tile_col);
- vp9_jobq_dequeue(&jobq);
- vp9_jobq_deinit(&jobq);

+ // No job queue - AOM handles internally
+ uint32_t decodingTaskQueueDepth;  // placeholder, unused
```

### Design Rationale

1. **Removed job queue** - AOM manages its own internal parallelism
2. **Placeholder field** - `decodingTaskQueueDepth` kept for API compatibility but unused
3. **Simplified pipeline** - No application-level job scheduling

---

## 6. Reference Frame / DPB Changes

### Reference (libvpx VP9)

VP9 manages reference frames explicitly:

```c
// vp9_decoder.c
static void swap_frame_buffers(VP9Decoder *pbi) {
  int ref_index = 0, mask;
  VP9_COMMON *const cm = &pbi->common;
  BufferPool *const pool = cm->buffer_pool;
  RefCntBuffer *const frame_bufs = cm->buffer_pool->frame_bufs;

  for (mask = pbi->refresh_frame_flags; mask; mask >>= 1) {
    const int old_idx = cm->ref_frame_map[ref_index];
    decrease_ref_count(old_idx, frame_bufs, pool);
    // ... update ref_frame_map ...
  }
  cm->frame_to_show = get_frame_new_buffer(cm);
}

// Reference counting
static INLINE void decrease_ref_count(int idx, RefCntBuffer *const frame_bufs,
                                      BufferPool *const pool) {
  if (idx >= 0 && frame_bufs[idx].ref_count > 0) {
    --frame_bufs[idx].ref_count;
    if (!frame_bufs[idx].released && frame_bufs[idx].ref_count == 0 &&
        frame_bufs[idx].raw_frame_buffer.priv) {
      pool->release_fb_cb(pool->cb_priv, &frame_bufs[idx].raw_frame_buffer);
      frame_bufs[idx].released = 1;
    }
  }
}
```

### Custom (AV1 AOM)

AV1 uses AOM's internal DPB management:

```c
// av1_internals.h
typedef struct _Av1AomDecoder {
    // ...
    uint32_t frame_count;  // decoded frames since last reset
    // ...
} Av1AomDecoder;
```

The custom API doesn't expose reference frame management - AOM handles it internally.

### Key Files Changed

| File | Change Type | Description |
|------|-------------|-------------|
| `vp9/decoder/vp9_decoder.c` | REFERENCE | `swap_frame_buffers()`, `decrease_ref_count()` |
| `vp9/common/vp9_onyxc_int.h` | REFERENCE | `RefCntBuffer`, `FRAME_BUFFERS` |

### Diff Summary

```diff
- RefCntBuffer frame_bufs[FRAME_BUFFERS];
- int ref_frame_map[REF_FRAMES];
- int next_ref_frame_map[REF_FRAMES];
- decrease_ref_count(idx, frame_bufs, pool);
- swap_frame_buffers(pbi);

+ // AOM manages DPB internally
+ uint32_t frame_count;  // just tracking decoded frame count
```

### Design Rationale

1. **Hidden DPB** - AOM manages reference frames internally
2. **Simplified tracking** - Only `frame_count` tracked externally
3. **Removed reference controls** - No `vp9_set_reference_dec()`, `vp9_copy_reference_dec()`

---

## 7. Bitstream Parsing Changes

### Reference (libvpx VP9)

VP9 uses frame-based parsing:

```c
// vp9_decodeframe.c
void vp9_decode_frame(VP9Decoder *pbi, const uint8_t *data,
                      const uint8_t *data_end, const uint8_t **p_data_end) {
  // Read uncompressed header
  // Read compressed header  
  // Parse tiles
}

// Superframe parsing
vpx_codec_err_t vp9_parse_superframe_index(const uint8_t *data, size_t data_sz,
                                           uint32_t sizes[8], int *count,
                                           vpx_decrypt_cb decrypt_cb,
                                           void *decrypt_state) {
  // Parse superframe index to get individual frame sizes
}
```

### Custom (AV1 AOM)

AV1 uses OBU (OBject Unit) parsing:

```c
// av1_tile_dec_unit.h
enum class ObuType : uint8_t {
    RESERVED               = 0,
    SEQUENCE_HEADER        = 1,
    TEMPORAL_DELIMITER     = 2,
    FRAME_HEADER           = 3,
    TILE_GROUP             = 4,
    METADATA               = 5,
    FRAME                  = 6,
    REDUNDANT_FRAME_HEADER = 7,
    TILE_LIST              = 8,
    PADDING                = 15,
};

enum class Av1TileDecodeUnitType {
    SessionStart,   // sequence header + frame header + tile group(s)
    NewFrame,       // frame header + tile group(s)
    TileGroupCont,  // tile group(s) only
    Error
};

class Av1TileDecodeUnit {
public:
    explicit Av1TileDecodeUnit(const uint8_t* data, size_t size);
    Av1TileDecodeUnitType type() const { return type_; }
    bool is_valid() const { return type_ != Av1TileDecodeUnitType::Error; }
private:
    Av1TileDecodeUnitType type_;
};
```

OBU parsing implementation:
```c
// av1_tile_dec_unit.cpp (implied)
inline Av1TileDecodeUnit::Av1TileDecodeUnit(const uint8_t* data, size_t size) {
    // Parse OBU header (1 byte)
    uint8_t b = data[pos++];
    ObuType obu_type = static_cast<ObuType>((b >> 3) & 0xF);
    bool ext = (b & 4) != 0;
    bool has_size = (b & 2) != 0;
    
    // Skip extension if present
    if (ext && pos < size) pos++;
    
    // LEB128 size decoding
    uint64_t obu_size = 0;
    for (int i = 0; i < 8 && pos < size; i++) {
        uint8_t v = data[pos++];
        obu_size |= uint64_t(v & 0x7F) << shift;
        if (!(v & 0x80)) break;
        shift += 7;
    }
}
```

### Key Files Changed

| File | Change Type | Description |
|------|-------------|-------------|
| `vp9/decoder/vp9_decodeframe.c` | REFERENCE | VP9 frame parsing |
| `av1_tile_dec_unit.h` | NEW | AV1 OBU/tile parsing |

### Diff Summary

```diff
- // VP9 frame parsing
- vp9_parse_superframe_index(...);
- vp9_decode_frame(...);
- // Uncompressed header + compressed header parsing

+ // AV1 OBU parsing
+ Av1TileDecodeUnit(data, size);
+ // OBU header parsing (type, extension, size)
+ // LEB128 size decoding
+ // SessionStart / NewFrame / TileGroupCont classification
```

### Design Rationale

1. **OBU-based parsing** - AV1 uses OBU structure vs VP9's frame structure
2. **Annex-B support** - Added `uses_annex_b` flag to prepend start codes
3. **Tile group handling** - New `TileGroupCont` type for streaming scenarios
4. **LEB128 decoding** - AV1 uses LEB128 for size fields

---

## 8. Post-Processing / Filtering

### Reference (libvpx VP9)

VP9 has loop filter and post-processing:

```c
// vp9_decoder.h
typedef struct VP9Decoder {
  // ...
  VPxWorker lf_worker;
  VP9LfSync lf_row_sync;
  // ...
} VP9Decoder;

// Loop filter allocation
int vp9_alloc_loop_filter(VP9_COMMON *cm) {
  cm->lf.lfm_stride = (cm->mi_cols + (MI_BLOCK_SIZE - 1)) >> 3;
  cm->lf.lfm = (LOOP_FILTER_MASK *)vpx_calloc(...);
}

// Loop filter worker
static void loop_filter_row(VPxWorker *const worker, void *data2) {
  // Apply loop filter to a row
}
```

### Custom (AV1 AOM)

AV1 loop filtering is handled internally by AOM - no external loop filter worker:

```c
// 17_compile_final_plan.md
// No loop filter worker in AV1 implementation
// Post-processing is handled by AOM internally
```

### Key Files Changed

| File | Change Type | Description |
|------|-------------|-------------|
| `vp9/common/vp9_loopfilter.h/c` | REFERENCE | Loop filter implementation |
| `vp9/common/vp9_thread_common.h` | REFERENCE | Loop filter sync |

### Diff Summary

```diff
- VPxWorker lf_worker;
- VP9LfSync lf_row_sync;
- vp9_loop_filter_init(cm);
- vp9_loop_filter_dealloc(&pbi->lf_row_sync);
- // Row-based loop filter threading

+ // Loop filter handled internally by AOM
+ // No external loop filter control
```

### Design Rationale

1. **Hidden loop filter** - AOM handles loop filtering internally
2. **No post-proc control** - No `vp9_ppflags_t` equivalent exposed
3. **Simplified API** - No loop filter parameter exposed to application

---

## 9. Output / Copy Path

### Reference (libvpx VP9)

VP9 output uses YV12 buffer config:

```c
// vp9_decoder.c
int vp9_get_raw_frame(VP9Decoder *pbi, YV12_BUFFER_CONFIG *sd,
                      vp9_ppflags_t *flags) {
  VP9_COMMON *const cm = &pbi->common;
  // ...
  *sd = *cm->frame_to_show;
  ret = 0;
}
```

### Custom (AV1 AOM)

AV1 output uses AOM's image structure:

```c
// 17_compile_final_plan.md
typedef struct _Av1AomDecOutputInfoBuffer {
    Av1AomDecProfile profile;
    Av1AomDecLevel level;
    uint32_t frameWidth;
    uint32_t frameHeight;
    uint32_t pixelRowByteAlignment;
    uint32_t maxPixelBitDepth;
    int usesAnnexB;
    int pixelMsbAlignFlag;
} Av1AomDecOutputInfoBuffer;

// Output sync
extern Av1AomDecReturnType av1AomDecSyncDecodeOutput(
    void* pContext,
    Av1AomDecOutputInfoBuffer* pInfo,
    uint64_t* pPts);
```

Output buffer registration:
```c
extern Av1AomDecReturnType av1AomDecSetDecodeOutput(
    void* pContext,
    void* pBuffer,
    uint32_t size);
```

### Key Files Changed

| File | Change Type | Description |
|------|-------------|-------------|
| `vp9_decoder.c` | REFERENCE | `vp9_get_raw_frame()` |
| `av1api_set_output.cpp` | NEW | Set external output buffer |
| `av1api_sync_output.cpp` | NEW | Sync and copy output |

### Diff Summary

```diff
- YV12_BUFFER_CONFIG *sd;
- *sd = *cm->frame_to_show;

+ Av1AomDecOutputInfoBuffer pInfo;
+ av1AomDecSyncDecodeOutput(ctx, &pInfo, &pts);
+ // Converts YV12 → planar image
```

### Design Rationale

1. **External buffer support** - `av1AomDecSetDecodeOutput()` allows app-provided buffers
2. **Output info** - Frame dimensions, bit depth, profile returned in output info
3. **Planar conversion** - `av1api_sync_output.cpp` converts AOM output to planar format

---

## 10. Error Handling & Edge Cases

### Reference (libvpx VP9)

VP9 uses detailed error handling:

```c
// vpx/vpx_codec.h
typedef enum {
  VPX_CODEC_OK,
  VPX_CODEC_ERROR,
  VPX_CODEC_MEM_ERROR,
  VPX_CODEC_ABI_MISMATCH,
  VPX_CODEC_INCAPABLE,
  VPX_CODEC_UNSUP_BITSTREAM,
  VPX_CODEC_UNSUP_FEATURE,
  VPX_CODEC_CORRUPT_FRAME,
  VPX_CODEC_INVALID_PARAM,
  VPX_CODEC_LIST_END
} vpx_codec_err_t;

// VP9-specific error info
struct vpx_internal_error_info {
  int error_code;
  int setjmp;
  jmp_buf jmp;
  char detail[80];
};

// Error handling in decoder
if (setjmp(cm->error.jmp)) {
  cm->error.setjmp = 0;
  pbi->ready_for_new_data = 1;
  release_fb_on_decoder_exit(pbi);
  return -1;
}
cm->error.setjmp = 1;
```

### Custom (AV1 AOM)

AV1 has custom error mapping:

```c
// av1_internals.h (from 16_plan_internals.md)
Av1AomDecReturnType av1AomDecMapError(aom_codec_err_t err) {
    // Maps AOM error codes to AV1 API codes
    // AOM_CODEC_OK → AV1_AOM_DEC_SUCCESS
    // AOM_CODEC_CORRUPT_FRAME → AV1_AOM_DEC_ERR_CORRUPT_FRAME
    // ...
}
```

Error codes:
```c
// 17_compile_final_plan.md
enum _Av1AomDecReturnType {
    AV1_AOM_DEC_SUCCESS = 0,
    AV1_AOM_DEC_ERR_INVALID_PARAM = -1,
    AV1_AOM_DEC_ERR_INVALID_CTX = -2,
    AV1_AOM_DEC_ERR_MEM_ALLOC = -3,
    AV1_AOM_DEC_ERR_UNSUP_BITSTREAM = -4,
    AV1_AOM_DEC_ERR_UNSUP_FEATURE = -5,
    AV1_AOM_DEC_ERR_CORRUPT_FRAME = -6,
    AV1_AOM_DEC_ERR_TIMEOUT = -7,  // kept but unused
    AV1_AOM_DEC_ERR_BUF_TOO_SMALL = -8,
    AV1_AOM_DEC_ERR_OBU_ERROR = -25,   // AV1-specific
    AV1_AOM_DEC_ERR_TILE_ERROR = -26,  // AV1-specific
};
```

### Key Files Changed

| File | Change Type | Description |
|------|-------------|-------------|
| `vpx/vpx_codec.h` | REFERENCE | Standard error codes |
| `vp9_decoder.c` | REFERENCE | `vpx_internal_error_info`, setjmp handling |
| `av1_internals.cpp` | NEW | Error mapping function |

### Diff Summary

```diff
- vpx_codec_err_t (standard)
- vpx_internal_error_info (setjmp-based)
- VPX_CODEC_OK, VPX_CODEC_ERROR, etc.

+ Av1AomDecReturnType (custom)
+ av1AomDecMapError() (mapping function)
+ AV1_AOM_DEC_SUCCESS, AV1_AOM_DEC_ERR_OBU_ERROR, etc.
+ AV1-specific: OBU_ERROR, TILE_ERROR
```

### Design Rationale

1. **Custom error codes** - AV1-specific errors (OBU, TILE)
2. **Error mapping** - `av1AomDecMapError()` converts AOM errors to custom codes
3. **Removed setjmp** - No longjmp-based error handling (AOM handles internally)
4. **Timeout removed** - `AV1_AOM_DEC_ERR_TIMEOUT` kept for API compatibility but unused

---

## Summary of Key Changes

| Area | VP9 Reference | AV1 Custom |
|------|---------------|------------|
| **API** | libvpx standard | Custom AV1 wrapper |
| **Memory** | Custom vpx_memalign, row-MT alloc | AOM internal + external FB support |
| **Threading** | Custom workers, job queue | AOM internal only |
| **Pipeline** | Blocking decode + get_frame | DecodeAu + SyncAu split |
| **Queue** | JobQueueRowMt | Removed (AOM internal) |
| **DPB** | Explicit ref counting | AOM internal |
| **Parsing** | Frame/superframe | OBU/tile group |
| **Loop Filter** | External worker | AOM internal |
| **Output** | YV12_BUFFER_CONFIG | Custom output info + planar |
| **Errors** | setjmp + vpx_codec_err_t | Custom mapping |

---

## Potential Issues and TODOs

### Bugs / Race Conditions

1. **Thread blocking risk** - `av1AomDecSyncAu()` may block indefinitely if AOM's internal threads have issues
2. **No thread control** - Application cannot control or query AOM's threading
3. **Memory pressure** - No way to query AOM's current memory usage

### Incomplete Areas

1. **`decodingTaskQueueDepth`** - Placeholder field, not functional
2. **`AV1_AOM_DEC_ERR_TIMEOUT`** - Defined but never used (should be removed per 18_final_review.md)
3. **Flush handling** - `av1AomDecFlush()` noted as "not directly supported by AOM" - implementation unclear

### Workarounds

1. **Annex-B start code** - Custom prepending in `av1AomDecPrepareInputBuffer()`
2. **External frame buffers** - Custom callback registration via `AV1_SET_FRAME_BUFFER_SIZE_CB`

### Missing from Reference

1. No equivalent to `vp9_copy_reference_dec()` / `vp9_set_reference_dec()`
2. No post-processing control (`vp9_ppflags_t`)
3. No frame metadata access (quantizers, loop filter levels)